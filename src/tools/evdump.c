/* evdump: print key events from all /dev/input/event* devices for N seconds (read-only). */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    struct pollfd p[32];
    char names[32][80];
    int n = 0;
    for (int i = 0; i < 32; i++) {
        char d[32];
        snprintf(d, sizeof d, "/dev/input/event%d", i);
        int fd = open(d, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        names[n][0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof names[n]), names[n]);
        printf("event%d: %s\n", i, names[n]);
        p[n].fd = fd; p[n].events = POLLIN; n++;
    }
    fflush(stdout);
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        if (poll(p, (nfds_t)n, 500) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            struct input_event ev;
            while (read(p[i].fd, &ev, sizeof ev) == sizeof ev)
                if (ev.type == EV_KEY || ev.type == EV_MSC)
                    printf("%s: type=%d code=%d (0x%x) value=%d\n", names[i], ev.type, ev.code, ev.code, ev.value);
        }
        fflush(stdout);
    }
    return 0;
}
