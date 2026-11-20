/* smbcommon: shares.conf parsing and libsmb2 connection helpers (Network Shares browser + smbgw). */
#pragma once
#include <stddef.h>

struct smb2_context;

/* shares.conf, one per line:   Name = smb://[DOMAIN;]user[:password]@host[:port]/[share[/folder]]
 * No share -> the entry lists the server's shares. No user -> guest. */
typedef struct {
    char name[96];
    char domain[64], user[96], pass[128];
    char host[128];
    int port;
    char share[128];
    char path[512];     /* folder inside the share, no leading/trailing slash */
} smb_entry;

int  smb_load_conf(const char *file, smb_entry *out, int max);   /* number of entries */
/* Connect to host/share; NULL on failure with a message in err. share "IPC$" for enumeration. */
struct smb2_context *smb_connect(const smb_entry *e, const char *share, char *err, size_t errlen);
/* Disk shares of the entry's server; returns count or -1. names[i] are malloc'ed. */
int  smb_list_shares(const smb_entry *e, char **names, int max, char *err, size_t errlen);
void url_decode(char *s);
void url_encode_path(const char *in, char *out, size_t n);   /* keeps '/', escapes the rest */
