/*
 * ncdebug: read / change the LG NetCast "debug status" (debug level) stored in the NVM EEPROM.
 *
 *   ncdebug status                 show the debug level (read-only)
 *   ncdebug dump [off] [len]       hex dump of the NVM EEPROM (read-only), off/len in hex
 *   ncdebug set <level> [--yes]    level: debug|event|release or 0|1|2
 *   ncdebug toggle [--yes]         RELEASE -> DEBUG, DEBUG/EVENT -> RELEASE
 *   options: --bus N --dev 0xNN --clk N   override the EEPROM's SIF location (other models)
 *
 * Without --yes, set/toggle only run the pre-write checks and print what they would do.
 *
 * Where the level lives (reverse-engineered from RELEASE and U-Boot, NetCast 4.5 / MT5398):
 *   - NVM byte 0x18a: 0 = DEBUG, 1 = EVENT, 2 = RELEASE (factory default).
 *     LG: SUMDSVC_CTRL_{Get,Set}DebugStatus / OSA_MD_GetDebugStatus -> DDI_NVM_Read/Write(0x18a, 1).
 *     OSA_MD_GetDebugStatus rewrites any value > 2 as 2. U-Boot (0x8184f0) reads 4 bytes at 0x14
 *     first; if they are ff ff ff ff (blank NVM) it ignores 0x18a and uses RELEASE.
 *   - NVM offsets < 0x33bb are an I2C EEPROM (not the eMMC): LG "NVM" I2C handle = ch 3, dev 0xA8,
 *     speed 1 -> DDI_CMNIO_I2C_* -> MediaTek SIF bus 0, clkdiv 380, 2-byte subaddress.
 *   - SIF transfers are MTAL ioctls on /dev/mtal (as in ncscreen.c):
 *     MTSIF_WriteMultipleSubAddr 0x40200C0B / MTSIF_ReadMultipleSubAddr 0x40200C0C,
 *     struct { bus, clkdiv, devaddr, u8 *subaddr, subaddr_len, buf*, count, pad }, the subaddress
 *     bytes big-endian ({hi, lo}, as LG's NVM code builds them). (Traced DDI_CMNIO_I2C_Read ->
 *     MTSIF_ReadMultipleSubAddr: field 3 is a pointer; ncscreen.c's {len, value} order only works
 *     there because it passes 0/0.)
 *
 * Safety measures for a write:
 *   - refuses unless /dev/mtal exists and the kernel console is ttyMT (MediaTek NetCast);
 *   - the address byte order is self-tested: one 32-byte block read of 0x180..0x19f must equal 32
 *     single-byte reads (a wrong byte order reads unrelated addresses), and the window must not be uniform;
 *   - the NVM must look initialised (0x14 != ff ff ff ff) and the current level must be 0..2;
 *   - refuses while an OTA is pending (SWU magic 0x20121205 @0x198 with 'S' @0x18b);
 *   - flock()s /tmp/ncdebug.lock and blocks SIGINT/SIGTERM/SIGHUP/SIGPIPE while writing;
 *   - writes ONE byte, waits for the EEPROM write cycle (ack polling), reads it back, and checks
 *     that every other byte of the 0x180..0x19f window is unchanged; up to 3 attempts.
 *   - does not touch RELEASE: LG's running software keeps its cached copy until the next boot,
 *     which is also when U-Boot and LG read the level, so the change takes effect after a power cycle.
 *
 * Effects of the level:
 *   RELEASE: silent boot, no UART console; a resumed snapshot that turns out bad is cleared and the
 *            TV cold-boots by itself.
 *   DEBUG:   UART console break-in with a backtick and verbose boot; a bad snapshot is NOT cleared
 *            (it hangs; recover over UART). LG userspace enables its debug prints.
 *   EVENT:   between the two (UART break-in on).
 * In RELEASE a failed check at boot also burns a fuse for good; DEBUG and EVENT don't. See
 * "What DEBUG does" in the README before changing the level.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MTSIF_WRITE 0x40200C0B
#define MTSIF_READ  0x40200C0C

#define OFF_VALIDMARK 0x0014
#define OFF_DEBUG     0x018a
#define OFF_SWU_FLAG  0x018b
#define OFF_SWU_MAGIC 0x0198
#define SWU_MAGIC     0x20121205u
#define WIN_START     0x0180
#define WIN_LEN       32

typedef struct { uint32_t bus, clkdiv, devaddr; uint8_t *subaddr; uint32_t subaddr_len; uint8_t *buf; uint32_t count, pad; } sif_req;

static int fd = -1;
static uint32_t g_bus = 0, g_dev = 0xA8, g_clk = 380;
static const char *names[] = {"DEBUG", "EVENT", "RELEASE"};

static const char *lvname(int v) { return v >= 0 && v <= 2 ? names[v] : "INVALID (LG treats it as RELEASE)"; }

static int nvm_read(uint32_t off, uint8_t *buf, uint32_t n) {
    uint8_t sa[2] = {(uint8_t)(off >> 8), (uint8_t)off};
    for (int t = 0; t < 5; t++) {
        sif_req r = {g_bus, g_clk, g_dev, sa, 2, buf, n, 0};
        if (ioctl(fd, MTSIF_READ, &r) == 0) return 0;
        usleep(5000);
    }
    return -1;
}

static int nvm_write1(uint32_t off, uint8_t v) {
    uint8_t sa[2] = {(uint8_t)(off >> 8), (uint8_t)off};
    sif_req w = {g_bus, g_clk, g_dev, sa, 2, &v, 1, 0};
    return ioctl(fd, MTSIF_WRITE, &w) == 0 ? 0 : -1;
}

/* After a write the EEPROM NACKs until its internal write cycle (<= 10 ms) ends. */
static int wait_ready(uint32_t off, uint8_t *out) {
    uint8_t sa[2] = {(uint8_t)(off >> 8), (uint8_t)off};
    for (int i = 0; i < 50; i++) {
        usleep(2000);
        sif_req r = {g_bus, g_clk, g_dev, sa, 2, out, 1, 0};
        if (ioctl(fd, MTSIF_READ, &r) == 0) return 0;
    }
    return -1;
}

static int open_mtal(void) {
    fd = open("/dev/mtal", O_RDWR);
    if (fd < 0) { perror("open /dev/mtal (not a MediaTek NetCast TV?)"); return -1; }
    return 0;
}

static int is_mtk(void) {
    char buf[1024] = "";
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "console=ttyMT") != NULL;
}

static void hexdump(uint32_t base, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 16) {
        printf("%04x:", base + i);
        for (uint32_t j = i; j < i + 16 && j < n; j++) printf(" %02x", p[j]);
        printf("\n");
    }
}

/* Self-test the address byte order and that the bus is stable. Returns 0 if OK. */
static int selftest(uint8_t win[WIN_LEN]) {
    uint8_t again[WIN_LEN];
    if (nvm_read(WIN_START, win, WIN_LEN) || nvm_read(WIN_START, again, WIN_LEN)) {
        fprintf(stderr, "NVM read failed (no EEPROM at SIF bus %u dev 0x%02x?)\n", g_bus, g_dev);
        return -1;
    }
    if (memcmp(win, again, WIN_LEN)) { fprintf(stderr, "two reads of the same window differ: unstable bus\n"); return -1; }
    int uniform = 1;
    for (int i = 1; i < WIN_LEN; i++) if (win[i] != win[0]) uniform = 0;
    if (uniform) { fprintf(stderr, "window 0x%x.. is uniform (0x%02x): cannot verify addressing\n", WIN_START, win[0]); return -1; }
    for (int i = 0; i < WIN_LEN; i++) {
        uint8_t b;
        if (nvm_read(WIN_START + i, &b, 1)) { fprintf(stderr, "single-byte read failed\n"); return -1; }
        if (b != win[i]) {
            fprintf(stderr, "addressing self-test failed at 0x%x (block 0x%02x, single 0x%02x): wrong device or byte order\n",
                    WIN_START + i, win[i], b);
            return -1;
        }
    }
    return 0;
}

static int read_u32(uint32_t off, uint32_t *v) {
    uint8_t b[4];
    if (nvm_read(off, b, 4)) return -1;
    *v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

static int status(void) {
    uint8_t win[WIN_LEN];
    uint32_t vm, swu;
    if (selftest(win) || read_u32(OFF_VALIDMARK, &vm) || read_u32(OFF_SWU_MAGIC, &swu)) return 1;
    int lvl = win[OFF_DEBUG - WIN_START];
    printf("NVM EEPROM: SIF bus %u, dev 0x%02x, addressing self-test OK\n", g_bus, g_dev);
    printf("valid mark @0x14: %08x%s\n", vm, vm == 0xffffffff ? "  (blank: U-Boot ignores 0x18a and uses RELEASE)" : "");
    printf("debug level @0x18a: %d = %s\n", lvl, lvname(lvl));
    if ((swu == SWU_MAGIC || swu == 0x05121220u) && win[OFF_SWU_FLAG - WIN_START] == 'S')
        printf("NOTE: an OTA update is pending (SWU mode set)\n");
    return 0;
}

static int parse_level(const char *s) {
    if (!strcasecmp(s, "debug") || !strcmp(s, "0")) return 0;
    if (!strcasecmp(s, "event") || !strcmp(s, "1")) return 1;
    if (!strcasecmp(s, "release") || !strcmp(s, "2")) return 2;
    return -1;
}

static int set_level(int target, int toggle, int yes) {
    if (!is_mtk()) { fprintf(stderr, "refusing: kernel console is not ttyMT (not a MediaTek NetCast TV)\n"); return 3; }
    int lk = open("/tmp/ncdebug.lock", O_RDWR | O_CREAT, 0600);
    if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB)) { fprintf(stderr, "another ncdebug is running\n"); return 1; }

    uint8_t before[WIN_LEN], after[WIN_LEN];
    memset(after, 0xff, sizeof after);
    uint32_t vm, swu;
    if (selftest(before)) { fprintf(stderr, "refusing to write\n"); return 3; }
    if (read_u32(OFF_VALIDMARK, &vm) || read_u32(OFF_SWU_MAGIC, &swu)) return 1;
    if (vm == 0xffffffff) { fprintf(stderr, "refusing: NVM valid mark is blank (U-Boot would ignore the level anyway)\n"); return 3; }
    if ((swu == SWU_MAGIC || swu == 0x05121220u) && before[OFF_SWU_FLAG - WIN_START] == 'S') {
        fprintf(stderr, "refusing: an OTA update is pending (SWU mode)\n"); return 3;
    }
    int cur = before[OFF_DEBUG - WIN_START];
    if (cur > 2) { fprintf(stderr, "refusing: current value 0x%02x at 0x18a is not 0..2 (unexpected layout)\n", cur); return 3; }
    if (toggle) target = cur == 2 ? 0 : 2;
    printf("current: %d = %s\ntarget:  %d = %s\n", cur, names[cur], target, names[target]);
    printf("window 0x%x before:\n", WIN_START);
    hexdump(WIN_START, before, WIN_LEN);
    if (target == cur) { printf("already set, nothing to do\n"); return 0; }
    if (!yes) { printf("checks passed; dry run only. Re-run with --yes to write 1 byte (0x18a = %d).\n", target); return 0; }

    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGINT); sigaddset(&blk, SIGTERM); sigaddset(&blk, SIGHUP); sigaddset(&blk, SIGPIPE);
    sigprocmask(SIG_BLOCK, &blk, &old);

    int ok = 0;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        uint8_t rb = 0xff;
        if (nvm_write1(OFF_DEBUG, (uint8_t)target)) { fprintf(stderr, "attempt %d: SIF write failed\n", attempt); usleep(20000); continue; }
        if (wait_ready(OFF_DEBUG, &rb)) { fprintf(stderr, "attempt %d: EEPROM did not come back after the write\n", attempt); continue; }
        if (rb != target) { fprintf(stderr, "attempt %d: read back 0x%02x (write-protected?)\n", attempt, rb); usleep(20000); continue; }
        ok = 1;
    }
    int rc = 0;
    if (nvm_read(WIN_START, after, WIN_LEN)) { fprintf(stderr, "post-write read failed\n"); rc = 1; }
    else {
        for (int i = 0; i < WIN_LEN; i++)
            if (WIN_START + i != OFF_DEBUG && after[i] != before[i]) {
                fprintf(stderr, "WARNING: byte 0x%x changed %02x -> %02x\n", WIN_START + i, before[i], after[i]);
                rc = 4;
            }
        printf("window 0x%x after:\n", WIN_START);
        hexdump(WIN_START, after, WIN_LEN);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    if (!ok) {
        fprintf(stderr, "FAILED to set the level; 0x18a now reads 0x%02x (was %d)\n", after[OFF_DEBUG - WIN_START], cur);
        return rc ? rc : 1;
    }
    printf("OK: debug level is now %d = %s. It takes effect at the next cold boot (power cycle).\n"
           "LG's menus show the old value until then. To undo: ncdebug set %s --yes\n", target, names[target], names[cur]);
    return rc;
}

static int dump(uint32_t off, uint32_t len) {
    uint8_t buf[256];
    while (len) {
        uint32_t n = len > sizeof buf ? sizeof buf : len;
        if (nvm_read(off, buf, n)) { fprintf(stderr, "read failed at 0x%x\n", off); return 1; }
        hexdump(off, buf, n);
        off += n; len -= n;
    }
    return 0;
}

static int usage(const char *a0) {
    fprintf(stderr, "usage: %s [--bus N --dev 0xNN --clk N] status | dump [off] [len] | set debug|event|release [--yes] | toggle [--yes]\n", a0);
    return 2;
}

int main(int argc, char **argv) {
    int yes = 0;
    const char *pos[4] = {0};
    int np = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) yes = 1;
        else if (!strcmp(argv[i], "--bus") && i + 1 < argc) g_bus = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) g_dev = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--clk") && i + 1 < argc) g_clk = strtoul(argv[++i], 0, 0);
        else if (argv[i][0] == '-') return usage(argv[0]);
        else if (np < 4) pos[np++] = argv[i];
    }
    if (!np) return usage(argv[0]);
    if (open_mtal()) return 1;
    if (!strcmp(pos[0], "status")) return status();
    if (!strcmp(pos[0], "dump")) {
        uint32_t off = np > 1 ? strtoul(pos[1], 0, 16) : 0, len = np > 2 ? strtoul(pos[2], 0, 16) : 0x200;
        return dump(off, len);
    }
    if (!strcmp(pos[0], "toggle")) return set_level(0, 1, yes);
    if (!strcmp(pos[0], "set") && np > 1) {
        int t = parse_level(pos[1]);
        if (t < 0) return usage(argv[0]);
        return set_level(t, 0, yes);
    }
    return usage(argv[0]);
}
