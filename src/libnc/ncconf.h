/* ncconf: C access to netcast.conf (the one settings file on the USB stick, see apps/ncconf.sh) and the
 * [settings] switches, shared by the Settings app (ncsettings) and the remote API (tools/ncapi).
 *
 * Every write changes ONE key: it reads the file, replaces that key in its section (keeping everything
 * else byte for byte), writes <conf>.tmp, fsyncs and renames it over the original, all under an flock on
 * <rundir>/ncconf.lock. So the Settings app and the API can change settings at the same time without
 * losing each other's changes. */
#ifndef NCCONF_H
#define NCCONF_H
#include <stddef.h>

const char *nc_rundir(void);       /* $NC_RUNDIR or /tmp: session state files (ncmenu.*, ncapp.fg, ...) */

/* KEY=value from [section] (section case-insensitive, key case-sensitive, last one wins, value trimmed).
 * Returns 1 if found, 0 if not (out = ""). */
int ncconf_get(const char *conf, const char *section, const char *key, char *out, size_t n);
/* Set KEY=value in [section] (created at the end if missing). 0 ok, -1 error. */
int ncconf_set(const char *conf, const char *section, const char *key, const char *value);

/* The [settings] switches. */
typedef struct {
    const char *key, *title, *desc;
    int def;
    const int *opts;
    int nopts;
    int api_readonly;              /* the API can read but not change it */
} ncs_def;
extern const ncs_def ncs_defs[];
extern const int ncs_ndefs;

const ncs_def *ncs_find(const char *key);
int ncs_valid(const ncs_def *d, int v);
int ncs_get(const char *conf, const ncs_def *d);           /* value, or the default if missing/invalid */
int ncs_set(const char *conf, const ncs_def *d, int v);    /* validate + write; 0 ok, -1 error */
/* Apply a setting now (restart ncscreen saver / ncnetkeep / ncapi, run ncnosleep / nclite).
 * tools = the netcast/tools directory. Uses system(). */
void ncs_apply(const ncs_def *d, int v, const char *tools);

#endif
