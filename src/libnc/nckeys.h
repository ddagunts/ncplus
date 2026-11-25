/* nckeys: remote key names and key injection into an evdev node, shared by tools/nckey and tools/ncapi.
 *
 * Injection write()s input_events to the node (kernel evdev_write -> input_inject_event), so every reader
 * of the device (LG's procmgr, libnc apps, ncscreen) sees it exactly like a key from the remote.
 * Codes 897/898 (IN-STOP / power-only paths in LG's SUMODE_DoHotKey) are always refused: IN-STOP resets
 * the set to its shipping state. */
#ifndef NCKEYS_H
#define NCKEYS_H

#define NCK_DEFAULT_DEV "/dev/input/event2"   /* "LGE RCU": the remote's keys */
#define NCK_MAX 0x3ff                         /* LG uses codes up to 0x3xx */

typedef struct { const char *name; int code; const char *label; } nck_name;
extern const nck_name nck_names[];
extern const int nck_nnames;

int nck_lookup(const char *name);          /* name (case-insensitive) or number -> code; -1 if unknown */
const char *nck_name_of(int code);         /* first name for a code, or NULL */
int nck_refused(int code);                 /* 1 for codes that must never be injected */
int nck_valid(int code);                   /* 1..NCK_MAX and not refused */

typedef struct {
    int fd;
    int have_bits;                         /* EVIOCGBIT worked (a real evdev node) */
    unsigned char bits[NCK_MAX / 8 + 1];
    char name[80];
} nck_dev;

int nck_open(nck_dev *d, const char *path, int write);   /* 0 ok, -1 errno set */
void nck_close(nck_dev *d);
int nck_declared(const nck_dev *d, int code); /* 1 declared, 0 not, -1 unknown (not an evdev node) */
int nck_emit(const nck_dev *d, int code, int value);     /* one EV_KEY + SYN_REPORT; refuses 897/898 */

#endif
