/* sniff: ptrace ONE thread, capture its next N ioctls with a given cmd, dump the arg struct and
 * the buffer it points to (word 5). Read-only; detaches when done or after a timeout.
 *   sniff <tid> <cmd-hex> [count] [timeout-s] */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <asm/ptrace.h>
static pid_t tid;
static void bye(int s) { (void)s; kill(tid, SIGSTOP); ptrace(PTRACE_DETACH, tid, 0, 0); kill(tid, SIGCONT); _exit(3); }
static long peek(unsigned long a) { return ptrace(PTRACE_PEEKDATA, tid, (void *)a, 0); }
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    tid = atoi(argv[1]);
    unsigned long cmd = strtoul(argv[2], 0, 16);
    int want = argc > 3 ? atoi(argv[3]) : 1, secs = argc > 4 ? atoi(argv[4]) : 20, got = 0;
    signal(SIGALRM, bye); alarm(secs);
    if (ptrace(PTRACE_ATTACH, tid, 0, 0) < 0) { perror("attach"); return 1; }
    waitpid(tid, 0, __WALL);
    int entering = 1;
    while (got < want) {
        if (ptrace(PTRACE_SYSCALL, tid, 0, 0) < 0) break;
        int st;
        if (waitpid(tid, &st, __WALL) < 0 || WIFEXITED(st)) break;
        struct pt_regs r;
        if (ptrace(PTRACE_GETREGS, tid, 0, &r) < 0) break;
        if (entering && r.ARM_r7 == 54 && (unsigned long)r.ARM_r1 == cmd) {
            unsigned long a = r.ARM_r2;
            printf("ioctl(%ld, %08lx, %08lx):", r.ARM_r0, cmd, a);
            unsigned long w[8];
            for (int i = 0; i < 8; i++) { w[i] = peek(a + 4 * i); printf(" %08lx", w[i]); }
            unsigned long buf = w[5], n = w[6] & 0xffff;
            printf("\n   buf@%08lx (%lu bytes):", buf, n);
            for (unsigned long i = 0; i < n && i < 16; i++) printf(" %02lx", (peek(buf + i) & 0xff));
            printf("\n");
            fflush(stdout);
            got++;
        }
        entering = !entering;
    }
    ptrace(PTRACE_DETACH, tid, 0, 0);
    return 0;
}
