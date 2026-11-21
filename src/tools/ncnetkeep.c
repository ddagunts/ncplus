/* ncnetkeep: keep the TV reachable on Wi-Fi.
 *
 * The TV's Broadcom Wi-Fi firmware drops every group-addressed (broadcast/multicast) frame: the radio
 * receives them (wl counter rxdfrmmcast climbs) but none reach Linux (rxmulti, wlan0 multicast = 0).
 * Unicast works both ways, and so does sending broadcasts. So the TV never hears ARP requests: once
 * the gateway's ARP entry for the TV expires (FreeBSD/pfSense: 20 min, and its re-check is a
 * broadcast) nothing can reach the TV until it sends an ARP request itself (its next DHCP renew,
 * up to 5 min later). Measured: up 20 min / down 5 min, around the clock.
 *
 * Fix: every few seconds send our own ARP requests. A host that receives an ARP request for its
 * own address (re)creates its entry for the sender, so the gateway and the LAN peers we talk to
 * always know our MAC. A gratuitous ARP also refreshes existing entries on every other host.
 *
 *   ncnetkeep [-i IFACE] [-t SECONDS] [-v]
 *     -i  interface (default: the one with the default route, else wlan0)
 *     -t  interval (default 30)
 *     -v  log every round (default: only changes of address/gateway)
 * RAM only, nothing is written anywhere; runs until killed.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXPEERS 16

struct arp_pkt {
    uint8_t dst[6], src[6];
    uint16_t type;
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} __attribute__((packed));

static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static int verbose;

__attribute__((format(printf, 1, 2))) static void logmsg(const char *fmt, ...) {
    struct timespec ts;
    va_list ap;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("[%ld] ", (long)ts.tv_sec);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* interface of the default route (and the gateway) from /proc/net/route */
static int default_route(char *ifn, size_t n, uint32_t *gw) {
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], name[IFNAMSIZ];
    unsigned dst, g, flags;
    int found = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%15s %x %x %x", name, &dst, &g, &flags) != 4) continue;
        if (dst != 0 || !(flags & 2)) continue;   /* RTF_GATEWAY */
        if (*ifn && strcmp(ifn, name)) continue;
        snprintf(ifn, n, "%s", name);
        *gw = g;                                  /* network byte order, as in the file */
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

/* complete ARP entries on ifn: our LAN peers */
static int arp_peers(const char *ifn, uint32_t *ip, uint8_t (*mac)[6], int max) {
    FILE *f = fopen("/proc/net/arp", "r");
    char line[256], a[32], m[32], dev[IFNAMSIZ];
    unsigned ht, fl;
    int n = 0;
    if (!f) return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %x %x %31s %*s %15s", a, &ht, &fl, m, dev) != 5) continue;
        if (strcmp(dev, ifn) || !(fl & 2)) continue;   /* ATF_COM */
        struct in_addr in;
        unsigned b[6];
        if (!inet_aton(a, &in) || sscanf(m, "%x:%x:%x:%x:%x:%x", b, b + 1, b + 2, b + 3, b + 4, b + 5) != 6) continue;
        ip[n] = in.s_addr;
        for (int i = 0; i < 6; i++) mac[n][i] = (uint8_t)b[i];
        n++;
    }
    fclose(f);
    return n;
}

static int send_arp(int sk, int ifindex, const uint8_t *me, uint32_t myip, const uint8_t *dst, uint32_t tip) {
    struct arp_pkt p;
    memcpy(p.dst, dst, 6);
    memcpy(p.src, me, 6);
    p.type = htons(0x0806);
    p.htype = htons(1);
    p.ptype = htons(0x0800);
    p.hlen = 6;
    p.plen = 4;
    p.op = htons(1);                 /* request */
    memcpy(p.sha, me, 6);
    memcpy(p.spa, &myip, 4);
    memset(p.tha, 0, 6);
    memcpy(p.tpa, &tip, 4);
    struct sockaddr_ll ll;
    memset(&ll, 0, sizeof ll);
    ll.sll_family = AF_PACKET;
    ll.sll_protocol = htons(0x0806);
    ll.sll_ifindex = ifindex;
    ll.sll_halen = 6;
    memcpy(ll.sll_addr, dst, 6);
    return (int)sendto(sk, &p, sizeof p, 0, (struct sockaddr *)&ll, sizeof ll);
}

int main(int argc, char **argv) {
    char want[IFNAMSIZ] = "";
    int interval = 30;
    for (int c; (c = getopt(argc, argv, "i:t:v")) != -1;) {
        if (c == 'i') snprintf(want, sizeof want, "%s", optarg);
        else if (c == 't') interval = atoi(optarg);
        else if (c == 'v') verbose = 1;
        else { fprintf(stderr, "usage: ncnetkeep [-i IFACE] [-t SECONDS] [-v]\n"); return 2; }
    }
    if (interval < 1) interval = 1;
    signal(SIGPIPE, SIG_IGN);
    int sk = socket(AF_PACKET, SOCK_RAW, htons(0x0806));
    int ctl = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0 || ctl < 0) { perror("ncnetkeep: socket"); return 1; }
    char last[128] = "";
    for (;;) {
        char ifn[IFNAMSIZ], state[128], a[32], g[32];
        uint32_t gw = 0, myip = 0;
        uint8_t me[6];
        int ifindex = 0;
        snprintf(ifn, sizeof ifn, "%s", want);
        int have_gw = default_route(ifn, sizeof ifn, &gw);
        if (!*ifn) snprintf(ifn, sizeof ifn, "wlan0");
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifn);
        if (ioctl(ctl, SIOCGIFINDEX, &ifr) == 0) ifindex = ifr.ifr_ifindex;
        if (ifindex && ioctl(ctl, SIOCGIFADDR, &ifr) == 0)
            myip = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
        if (myip && ioctl(ctl, SIOCGIFHWADDR, &ifr) == 0) memcpy(me, ifr.ifr_hwaddr.sa_data, 6);
        else myip = 0;

        if (!myip) snprintf(state, sizeof state, "%s: no address, waiting", ifn);
        else {
            snprintf(a, sizeof a, "%s", inet_ntoa(*(struct in_addr *)&myip));
            snprintf(g, sizeof g, "%s", have_gw ? inet_ntoa(*(struct in_addr *)&gw) : "none");
            snprintf(state, sizeof state, "%s %s gateway %s", ifn, a, g);
        }
        if (strcmp(state, last)) { logmsg("%s", state); snprintf(last, sizeof last, "%s", state); }

        if (myip) {
            uint32_t ip[MAXPEERS];
            uint8_t mac[MAXPEERS][6];
            int n = arp_peers(ifn, ip, mac, MAXPEERS), sent = 0, err = 0;
            /* gratuitous ARP: refreshes every host that still has an entry for us */
            if (send_arp(sk, ifindex, me, myip, bcast, myip) > 0) sent++; else err = errno;
            /* the gateway: broadcast, so it works even before we have its MAC */
            if (have_gw) { if (send_arp(sk, ifindex, me, myip, bcast, gw) > 0) sent++; else err = errno; }
            /* the peers we talk to, unicast: each one (re)creates its entry for us */
            for (int i = 0; i < n; i++) {
                if (have_gw && ip[i] == gw) continue;
                if (send_arp(sk, ifindex, me, myip, mac[i], ip[i]) > 0) sent++; else err = errno;
            }
            if (err) logmsg("send failed: %s", strerror(err));
            else if (verbose) logmsg("sent %d ARP requests", sent);
        }
        sleep((unsigned)interval);
    }
}
