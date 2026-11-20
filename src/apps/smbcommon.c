#include "smbcommon.h"
#include <ctype.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>

void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

void url_encode_path(const char *in, char *out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || strchr("/-_.~", c)) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0;
}

static int parse_url(const char *url, smb_entry *e) {
    if (strncasecmp(url, "smb://", 6)) return -1;
    char u[1024];
    snprintf(u, sizeof u, "%s", url + 6);
    char *hostpart = u, *at = strrchr(u, '@');
    char *slash = strchr(u, '/');
    if (at && (!slash || at < slash)) {
        *at = 0;
        hostpart = at + 1;
        char *ui = u, *semi = strchr(ui, ';');
        if (semi) { *semi = 0; snprintf(e->domain, sizeof e->domain, "%s", ui); ui = semi + 1; }
        char *colon = strchr(ui, ':');
        if (colon) { *colon = 0; snprintf(e->pass, sizeof e->pass, "%s", colon + 1); }
        snprintf(e->user, sizeof e->user, "%s", ui);
        url_decode(e->domain); url_decode(e->user); url_decode(e->pass);
    }
    slash = strchr(hostpart, '/');
    if (slash) *slash = 0;
    char *colon = strchr(hostpart, ':');
    if (colon) { *colon = 0; e->port = atoi(colon + 1); }
    snprintf(e->host, sizeof e->host, "%s", hostpart);
    if (slash) {
        char *rest = slash + 1, *s2 = strchr(rest, '/');
        if (s2) { *s2 = 0; snprintf(e->path, sizeof e->path, "%s", s2 + 1); }
        snprintf(e->share, sizeof e->share, "%s", rest);
        url_decode(e->share); url_decode(e->path);
        size_t l = strlen(e->path);
        while (l && e->path[l - 1] == '/') e->path[--l] = 0;
    }
    return e->host[0] ? 0 : -1;
}

int smb_load_conf(const char *file, smb_entry *out, int max) {
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char line[1200];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strstr(line, "smb://");
        if (!eq) continue;
        smb_entry e;
        memset(&e, 0, sizeof e);
        char *sep = strchr(line, '=');
        if (sep && sep < eq) { *sep = 0; snprintf(e.name, sizeof e.name, "%s", line); trim(e.name); }
        if (parse_url(eq, &e) != 0) continue;
        if (!e.name[0]) snprintf(e.name, sizeof e.name, "%s%s%s", e.host, e.share[0] ? "/" : "", e.share);
        out[n++] = e;
    }
    fclose(f);
    return n;
}

struct smb2_context *smb_connect(const smb_entry *e, const char *share, char *err, size_t errlen) {
    struct smb2_context *c = smb2_init_context();
    if (!c) { snprintf(err, errlen, "out of memory"); return NULL; }
    smb2_set_security_mode(c, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(c, 15);
    smb2_set_user(c, e->user[0] ? e->user : "Guest");
    smb2_set_password(c, e->pass);
    if (e->domain[0]) smb2_set_domain(c, e->domain);
    char server[160];
    if (e->port) snprintf(server, sizeof server, "%s:%d", e->host, e->port);
    else snprintf(server, sizeof server, "%s", e->host);
    if (smb2_connect_share(c, server, share, NULL) < 0) {
        const char *why = smb2_get_error(c);
        if (strstr(why, "LOGON_FAILURE") || strstr(why, "Error reply") || strstr(why, "ACCESS_DENIED"))
            why = "login failed (check user/password in shares.conf)";
        else if (strstr(why, "connect failed") || strstr(why, "Connection refused") || strstr(why, "timed out"))
            why = "server not reachable";
        snprintf(err, errlen, "%s/%s: %s", e->host, share, why);
        smb2_destroy_context(c);
        return NULL;
    }
    return c;
}

struct enum_state { int done, status, n, max; char **names; };

static void enum_cb(struct smb2_context *smb2, int status, void *data, void *priv) {
    struct enum_state *st = priv;
    st->done = 1;
    st->status = status;
    if (status) return;
    struct srvsvc_NetrShareEnum_rep *rep = data;
    struct srvsvc_SHARE_INFO_1_CONTAINER *l1 = &rep->ses.ShareInfo.Level1;
    for (uint32_t i = 0; i < l1->EntriesRead && st->n < st->max; i++) {
        struct srvsvc_SHARE_INFO_1 *si = &l1->Buffer->share_info_1[i];
        if ((si->type & 3) != SHARE_TYPE_DISKTREE || (si->type & SHARE_TYPE_HIDDEN)) continue;
        if (!si->netname.utf8) continue;
        size_t l = strlen(si->netname.utf8);
        if (l && si->netname.utf8[l - 1] == '$') continue;   /* admin shares */
        st->names[st->n++] = strdup(si->netname.utf8);
    }
    smb2_free_data(smb2, rep);
}

int smb_list_shares(const smb_entry *e, char **names, int max, char *err, size_t errlen) {
    struct smb2_context *c = smb_connect(e, "IPC$", err, errlen);
    if (!c) return -1;
    struct enum_state st = {0, 0, 0, max, names};
    if (smb2_share_enum_async(c, SHARE_INFO_1, enum_cb, &st) != 0) {
        snprintf(err, errlen, "share list: %s", smb2_get_error(c));
        smb2_destroy_context(c);
        return -1;
    }
    for (int waited = 0; !st.done && waited < 15000; ) {
        struct pollfd p = {smb2_get_fd(c), (short)smb2_which_events(c), 0};
        int r = poll(&p, 1, 250);
        if (r < 0) break;
        if (r == 0) { waited += 250; smb2_service(c, 0); continue; }
        if (smb2_service(c, p.revents) < 0) { snprintf(err, errlen, "share list: %s", smb2_get_error(c)); break; }
    }
    if (!st.done) { if (!err[0]) snprintf(err, errlen, "share list: timeout"); st.n = -1; }
    else if (st.status) { snprintf(err, errlen, "share list: %s", smb2_get_error(c)); st.n = -1; }
    smb2_disconnect_share(c);
    smb2_destroy_context(c);
    return st.n;
}
