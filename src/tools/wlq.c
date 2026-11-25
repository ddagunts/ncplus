/* wlq: minimal Broadcom wl ioctl client (the TV ships no `wl` tool).
 *   wlq [-i wlan0] get <iovar>          integer iovar via WLC_GET_VAR, e.g. arpoe arp_ol bcn_li_dtim allmulti
 *   wlq [-i wlan0] getbuf <iovar> [n]   hex dump of the first n (64) bytes of an iovar
 *   wlq [-i wlan0] ioc <cmd> [n]        raw WLC_GET ioctl number, hex dump (e.g. 85 = WLC_GET_PM)
 *   wlq [-i wlan0] set <iovar> <int>    integer iovar via WLC_SET_VAR (runtime only, nothing is saved)
 *   wlq [-i wlan0] setioc <cmd> <int>   raw WLC_SET ioctl with one int (e.g. 86 = WLC_SET_PM)
 *   wlq [-i wlan0] getkey <index>       WLC_GET_KEY: wl_wsec_key_t (index, len, algo, flags, rx IV)
 *   wlq [-i wlan0] bss                  the AP's RSN/WPA IE: group and pairwise ciphers, AKM, DTIM
 *   wlq [-i wlan0] cdiff [s]            which `counters` words changed over s (10) seconds
 *   wlq [-i wlan0] dump                 the values relevant to the Wi-Fi drop investigation */
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SIOCDEVPRIVATE 0x89F0
#define WLC_GET_MAGIC 0
#define WLC_GET_VAR 262
#define WLC_SET_VAR 263
#define WLC_MAGIC 0x14e46c77

typedef struct { unsigned cmd; void *buf; unsigned len; uint8_t set; unsigned used, needed; } wl_ioctl_t;

static const char *ifn = "wlan0";
static int sk = -1;

static int wl(unsigned cmd, void *buf, unsigned len, int set) {
    wl_ioctl_t io = {cmd, buf, len, (uint8_t)set, 0, 0};
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifn);
    ifr.ifr_data = (char *)&io;
    return ioctl(sk, SIOCDEVPRIVATE, &ifr);
}
static int getvar(const char *name, void *out, unsigned len) {
    char buf[1536];
    memset(buf, 0, sizeof buf);
    snprintf(buf, sizeof buf, "%s", name);
    if (len > sizeof buf) len = sizeof buf;
    unsigned need = (unsigned)strlen(name) + 1;
    int r = wl(WLC_GET_VAR, buf, len > need ? len : need + 4, 0);
    if (r == 0) memcpy(out, buf, len);
    return r;
}
static int setvar_int(const char *name, int v) {
    char buf[128];
    memset(buf, 0, sizeof buf);
    size_t n = strlen(name) + 1;
    memcpy(buf, name, n);
    memcpy(buf + n, &v, 4);
    return wl(WLC_SET_VAR, buf, (unsigned)(n + 4), 1);
}
static void hex(const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++) printf("%02x%s", p[i], (i % 16 == 15 || i + 1 == n) ? "\n" : " ");
}
static void show_int(const char *name) {
    int v = 0;
    if (getvar(name, &v, 4) == 0) printf("%-14s %d (0x%x)\n", name, v, v);
    else printf("%-14s unsupported\n", name);
}
static void show_ioc(const char *label, unsigned cmd) {
    int v = 0;
    if (wl(cmd, &v, 4, 0) == 0) printf("%-14s %d (0x%x)\n", label, v, v);
    else printf("%-14s ioctl %u failed\n", label, cmd);
}

int main(int argc, char **argv) {
    int a = 1;
    if (argc > 2 && !strcmp(argv[1], "-i")) { ifn = argv[2]; a = 3; }
    if (a >= argc) { fprintf(stderr, "usage: see source\n"); return 2; }
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    unsigned magic = 0;
    if (wl(WLC_GET_MAGIC, &magic, 4, 0) < 0 || magic != WLC_MAGIC) {
        fprintf(stderr, "wlq: %s is not a Broadcom wl interface (magic 0x%x)\n", ifn, magic);
        return 1;
    }
    const char *op = argv[a];
    if (!strcmp(op, "get") && a + 1 < argc) { show_int(argv[a + 1]); return 0; }
    if (!strcmp(op, "getbuf") && a + 1 < argc) {
        unsigned n = a + 2 < argc ? (unsigned)atoi(argv[a + 2]) : 64;
        uint8_t b[1536];
        if (n > sizeof b) n = sizeof b;
        if (getvar(argv[a + 1], b, n) < 0) { perror("getvar"); return 1; }
        hex(b, n);
        return 0;
    }
    if (!strcmp(op, "ioc") && a + 1 < argc) {
        unsigned n = a + 2 < argc ? (unsigned)atoi(argv[a + 2]) : 4;
        uint8_t b[1536] = {0};
        if (n > sizeof b) n = sizeof b;
        if (wl((unsigned)atoi(argv[a + 1]), b, n, 0) < 0) { perror("ioctl"); return 1; }
        hex(b, n);
        return 0;
    }
    if (!strcmp(op, "set") && a + 2 < argc) {
        int r = setvar_int(argv[a + 1], atoi(argv[a + 2]));
        if (r < 0) perror("setvar");
        show_int(argv[a + 1]);
        return r < 0;
    }
    if (!strcmp(op, "setioc") && a + 2 < argc) {
        int v = atoi(argv[a + 2]);
        int r = wl((unsigned)atoi(argv[a + 1]), &v, 4, 1);
        if (r < 0) perror("ioctl");
        return r < 0;
    }
    if (!strcmp(op, "getkey") && a + 1 < argc) {   /* WLC_GET_KEY (44): wl_wsec_key_t for key index n */
        uint8_t b[256] = {0};
        uint32_t idx = (uint32_t)atoi(argv[a + 1]);
        memcpy(b, &idx, 4);
        if (wl(44, b, 164, 0) < 0) { perror("getkey"); return 1; }
        hex(b, 164);
        return 0;
    }
    if (!strcmp(op, "bss")) {   /* WLC_GET_BSS_INFO (136): the associated AP's beacon IEs */
        static uint8_t b[2048];
        memset(b, 0, sizeof b);
        *(uint32_t *)b = sizeof b;
        if (wl(136, b, sizeof b, 0) < 0) { perror("bss_info"); return 1; }
        uint8_t *bi = b + 4;
        uint32_t ver, len, ielen; uint16_t ieoff;
        memcpy(&ver, bi, 4); memcpy(&len, bi + 4, 4); memcpy(&ieoff, bi + 116, 2); memcpy(&ielen, bi + 120, 4);
        printf("bss_info ver=%u len=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=%.*s dtim=%u ie_off=%u ie_len=%u\n",
               ver, len, bi[8], bi[9], bi[10], bi[11], bi[12], bi[13], bi[18] > 32 ? 32 : bi[18], bi + 19, bi[76],
               ieoff, ielen);
        if (ieoff < 100 || ieoff + ielen > sizeof b - 4) { hex(bi, 160); return 1; }
        static const char *cn[] = {"none", "WEP40", "TKIP", "rsvd", "CCMP", "WEP104", "BIP"};
        for (uint8_t *p = bi + ieoff, *e = p + ielen; p + 2 <= e && p + 2 + p[1] <= e; p += 2 + p[1]) {
            uint8_t *q = p + 2, *qe = q + p[1];
            int rsn = p[0] == 48, wpa = p[0] == 221 && p[1] >= 6 && !memcmp(q, "\x00\x50\xf2\x01", 4);
            if (!rsn && !wpa) continue;
            q += rsn ? 2 : 6;   /* RSN: version; WPA: OUI+type+version */
            printf("%s group=%s", rsn ? "RSN" : "WPA", q + 4 <= qe ? cn[q[3] < 7 ? q[3] : 3] : "?");
            q += 4;
            if (q + 2 <= qe) {
                unsigned n = q[0] | q[1] << 8; q += 2;
                printf(" pairwise=");
                for (unsigned i = 0; i < n && q + 4 <= qe; i++, q += 4) printf("%s%s", i ? "+" : "", cn[q[3] < 7 ? q[3] : 3]);
            }
            if (q + 2 <= qe) {
                unsigned n = q[0] | q[1] << 8; q += 2;
                printf(" akm=");
                for (unsigned i = 0; i < n && q + 4 <= qe; i++, q += 4) printf("%s%u", i ? "+" : "", q[3]);
            }
            if (rsn && q + 2 <= qe) printf(" caps=0x%04x", q[0] | q[1] << 8);
            printf("\n");
        }
        return 0;
    }
    if (!strcmp(op, "cdiff")) {   /* `counters` iovar twice, N s apart: which 32-bit words moved */
        int s = a + 1 < argc ? atoi(argv[a + 1]) : 10;
        static uint32_t c0[200], c1[200];
        if (getvar("counters", c0, sizeof c0) < 0) { perror("counters"); return 1; }
        sleep((unsigned)s);
        if (getvar("counters", c1, sizeof c1) < 0) { perror("counters"); return 1; }
        printf("counters ver=%u len=%u, words (index after the 4-byte header) that changed in %d s:\n",
               c1[0] & 0xffff, c1[0] >> 16, s);
        for (int i = 1; i < 200; i++)
            if (c1[i] != c0[i]) printf("  [%3d] %u -> %u (+%u)\n", i - 1, c0[i], c1[i], c1[i] - c0[i]);
        return 0;
    }
    if (!strcmp(op, "dump")) {
        show_ioc("PM", 85);              /* WLC_GET_PM: 0 off, 1 PS-Poll, 2 fast PS */
        show_ioc("wsec", 133);           /* WLC_GET_WSEC */
        show_ioc("wpa_auth", 164);       /* WLC_GET_WPA_AUTH */
        const char *iv[] = {"sup_wpa", "arpoe", "arp_ol", "arp_peerage", "ndoe", "allmulti", "bcn_li_dtim",
                            "bcn_li_bcn", "assoc_listen", "mpc", "wme_apsd", "pm2_sleep_ret", "roam_off",
                            "bcn_timeout", "dtim_wakeup", "pkt_filter_mode", "mkeep_alive", "keep_alive", 0};
        for (int i = 0; iv[i]; i++) show_int(iv[i]);
        char ver[256] = "";
        if (getvar("ver", ver, sizeof ver - 1) == 0) printf("ver            %s", ver);
        return 0;
    }
    fprintf(stderr, "wlq: bad arguments\n");
    return 2;
}
