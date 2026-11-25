/*
 * ncpreflight: read-only check that this TV is one fire.sh may boot another kernel on.
 *
 *   ncpreflight        prints "preflight: ok" and exits 0, or says why not and exits 1
 *
 * It reads a few bytes of the TV's settings EEPROM through /dev/mtal and has no write path.
 * Anything unexpected (no device, unstable or inconsistent reads, unknown contents) is a "no".
 * fire.sh depends on it: on a TV where it fails, a kernel that misbehaves can leave the set
 * permanently unbootable.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MTSIF_READ  0x40200C0C

#define OFF_VALIDMARK 0x0014
#define OFF_MODE      0x018a
#define WIN_START     0x0180
#define WIN_LEN       32

typedef struct { uint32_t bus, clkdiv, devaddr; uint8_t *subaddr; uint32_t subaddr_len; uint8_t *buf; uint32_t count, pad; } sif_req;

static int fd = -1;

static int nvm_read(uint32_t off, uint8_t *buf, uint32_t n) {
    uint8_t sa[2] = {(uint8_t)(off >> 8), (uint8_t)off};
    for (int t = 0; t < 5; t++) {
        sif_req r = {0, 380, 0xA8, sa, 2, buf, n, 0};
        if (ioctl(fd, MTSIF_READ, &r) == 0) return 0;
        usleep(5000);
    }
    return -1;
}

static int no(const char *why) {
    printf("preflight: NOT ok: %s\n", why);
    return 1;
}

int main(void) {
    uint8_t win[WIN_LEN], again[WIN_LEN], vm[4], b;
    int uniform = 1;

    fd = open("/dev/mtal", O_RDWR);
    if (fd < 0) return no("can't open /dev/mtal (not a MediaTek NetCast TV?)");
    if (nvm_read(WIN_START, win, WIN_LEN) || nvm_read(WIN_START, again, WIN_LEN)) return no("EEPROM read failed");
    if (memcmp(win, again, WIN_LEN)) return no("two reads of the same bytes differ");
    for (int i = 1; i < WIN_LEN; i++) if (win[i] != win[0]) uniform = 0;
    if (uniform) return no("EEPROM contents are uniform, can't check the addressing");
    for (int i = 0; i < WIN_LEN; i++) {
        if (nvm_read(WIN_START + i, &b, 1)) return no("single-byte read failed");
        if (b != win[i]) return no("block and single-byte reads differ (another EEPROM layout?)");
    }
    if (nvm_read(OFF_VALIDMARK, vm, 4)) return no("EEPROM read failed");
    if (vm[0] == 0xff && vm[1] == 0xff && vm[2] == 0xff && vm[3] == 0xff) return no("the EEPROM is blank");
    if (win[OFF_MODE - WIN_START] != 0) return no("this TV isn't set up for booting another kernel");
    printf("preflight: ok\n");
    return 0;
}
