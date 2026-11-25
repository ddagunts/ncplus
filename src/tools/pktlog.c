/* pktlog: passive Wi-Fi traffic logger for the periodic-drop investigation (read-only AF_PACKET).
 * Every <interval> seconds it prints frame counts by type (unicast to us, broadcast, multicast, sent),
 * and it prints every ARP, DHCP and EAPOL frame as it happens, with /proc/uptime timestamps.
 *   pktlog [ifname=wlan0] [interval_s=5]          (output on stdout, line-buffered) */
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static double uptime(void) {
    double t = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &t) != 1) t = 0; fclose(f); }
    return t;
}
static const char *ip4(const uint8_t *p, char *b) {
    sprintf(b, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}
static const char *ptype(int t) {
    switch (t) {
    case PACKET_HOST: return "in";
    case PACKET_BROADCAST: return "in-bc";
    case PACKET_MULTICAST: return "in-mc";
    case PACKET_OUTGOING: return "out";
    default: return "other";
    }
}

static void dhcp(double t, int pt, const uint8_t *u, int n) {
    /* u = UDP payload (BOOTP) */
    if (n < 240 || u[236] != 99 || u[237] != 130 || u[238] != 83 || u[239] != 99) return;
    static const char *mt[] = {"?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK", "RELEASE", "INFORM"};
    char a[16], b[16], c[16], s[16] = "-", rq[16] = "-";
    int type = 0;
    long lease = -1, t1 = -1, t2 = -1;
    for (int i = 240; i < n && u[i] != 255;) {
        if (u[i] == 0) { i++; continue; }
        if (i + 1 >= n) break;
        int o = u[i], l = u[i + 1];
        const uint8_t *v = u + i + 2;
        if (i + 2 + l > n) break;
        uint32_t w = l >= 4 ? (uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3] : 0;
        if (o == 53 && l >= 1) type = v[0];
        else if (o == 51 && l >= 4) lease = w;
        else if (o == 58 && l >= 4) t1 = w;
        else if (o == 59 && l >= 4) t2 = w;
        else if (o == 54 && l >= 4) ip4(v, s);
        else if (o == 50 && l >= 4) ip4(v, rq);
        i += 2 + l;
    }
    printf("%.1f DHCP %s %s xid=%02x%02x%02x%02x ciaddr=%s yiaddr=%s siaddr=%s server=%s req=%s lease=%ld t1=%ld t2=%ld\n",
           t, ptype(pt), type > 0 && type <= 8 ? mt[type] : "?", u[4], u[5], u[6], u[7],
           ip4(u + 12, a), ip4(u + 16, b), ip4(u + 20, c), s, rq, lease, t1, t2);
}

int main(int argc, char **argv) {
    const char *ifn = argc > 1 ? argv[1] : "wlan0";
    int iv = argc > 2 ? atoi(argv[2]) : 5;
    if (iv < 1) iv = 5;
    setvbuf(stdout, 0, _IOLBF, 0);
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_ll sl = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = (int)if_nametoindex(ifn)};
    if (!sl.sll_ifindex || bind(fd, (struct sockaddr *)&sl, sizeof sl) < 0) { perror("bind"); return 1; }
    printf("%.1f pktlog start on %s, summary every %d s\n", uptime(), ifn, iv);

    unsigned long cnt[5] = {0};  /* in, in-bc, in-mc, out, other */
    double next = uptime() + iv;
    uint8_t buf[2048];
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        double now = uptime();
        int ms = (int)((next - now) * 1000);
        if (ms < 0) ms = 0;
        if (poll(&p, 1, ms) > 0) {
            struct sockaddr_ll from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n >= 14) {
                int pt = from.sll_pkttype;
                cnt[pt == PACKET_HOST ? 0 : pt == PACKET_BROADCAST ? 1 : pt == PACKET_MULTICAST ? 2 : pt == PACKET_OUTGOING ? 3 : 4]++;
                uint16_t et = (uint16_t)(buf[12] << 8 | buf[13]);
                const uint8_t *l3 = buf + 14;
                int n3 = n - 14;
                double t = uptime();
                if (et == ETH_P_ARP && n3 >= 28) {
                    char a[16], b[16];
                    int op = l3[6] << 8 | l3[7];
                    printf("%.1f ARP %s %s %s (%02x:%02x:%02x:%02x:%02x:%02x) -> %s\n", t, ptype(pt),
                           op == 1 ? "who-has" : op == 2 ? "reply" : "op?", ip4(l3 + 14, a),
                           l3[8], l3[9], l3[10], l3[11], l3[12], l3[13], ip4(l3 + 24, b));
                } else if (et == 0x888e) {
                    printf("%.1f EAPOL %s type=%d len=%d\n", t, ptype(pt), n3 > 1 ? l3[1] : -1, n3);
                } else if (et == ETH_P_IP && n3 >= 28 && l3[9] == 17) {
                    int ihl = (l3[0] & 15) * 4;
                    if (n3 >= ihl + 8) {
                        const uint8_t *u = l3 + ihl;
                        int sp = u[0] << 8 | u[1], dp = u[2] << 8 | u[3];
                        if ((sp == 67 || sp == 68) && (dp == 67 || dp == 68)) dhcp(t, pt, u + 8, n3 - ihl - 8);
                    }
                }
            }
        }
        now = uptime();
        if (now >= next) {
            printf("%.1f sum in=%lu bc=%lu mc=%lu out=%lu other=%lu\n", now, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]);
            memset(cnt, 0, sizeof cnt);
            next += iv;
            if (next < now) next = now + iv;
        }
    }
}
