/* peek: read words from another process via ptrace (attach, PEEKDATA, detach). Read-only.
 *   peek <pid> <addr> [nwords]          addr in hex
 *   peek <pid> *<addr> [nwords]         dereference addr first (read the pointer stored there) */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: peek pid [*]addr [n]\n"); return 2; }
    pid_t pid = atoi(argv[1]);
    int deref = argv[2][0] == '*';
    unsigned long addr = strtoul(argv[2] + deref, 0, 16);
    int n = argc > 3 ? atoi(argv[3]) : 4;
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) { perror("attach"); return 1; }
    waitpid(pid, 0, __WALL);
    if (deref) {
        errno = 0;
        long p = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, 0);
        if (errno) { perror("peek"); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }
        printf("*%08lx = %08lx\n", addr, (unsigned long)p);
        addr = (unsigned long)p;
    }
    for (int i = 0; i < n; i++) {
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + 4 * i), 0);
        if (errno) { perror("peek"); break; }
        printf("%08lx: %08lx\n", addr + 4 * i, (unsigned long)w);
    }
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return 0;
}
