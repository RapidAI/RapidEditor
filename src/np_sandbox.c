/* np_sandbox.c - T10 sandbox policy gate (see header). */
#include "np_sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct np_sb_audit {
    np_sb_event_t *events;
    size_t cap, count, head;
    np_sb_clock_fn clock;
    void *clock_user;
};

const char *np_sb_verdict_name(np_sb_verdict_t v) {
    switch (v) {
        case NP_SB_ALLOW:         return "allow";
        case NP_SB_DENY_OFFLINE:  return "offline-denied";
        case NP_SB_DENY_DOMAIN:   return "domain-denied";
        case NP_SB_DENY_PATH:     return "path-denied";
        case NP_SB_DENY_TRAVERSAL:return "traversal-denied";
        case NP_SB_DENY_TOOL:     return "tool-denied";
        case NP_SB_DENY_RESOURCE: return "resource-denied";
        default:                  return "arg-denied";
    }
}

void np_sb_policy_defaults(np_sb_policy_t *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->allow_external_tools = true;   /* local tools allowed by default;
                                       * PUBLIC network still gated */
}

static void scpy(char *d, size_t cap, const char *s) {
    if (!cap) return;
    if (!s) { d[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
}

static bool ci_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!b[i]) return false;
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

/* ---- URL / domain -------------------------------------------------- */
bool np_sb_url_host(const char *url, char *host, size_t cap) {
    if (!url || !host || cap == 0) return false;
    host[0] = 0;
    const char *p = url;
    /* scheme:// */
    const char *sep = strstr(p, "://");
    if (!sep || sep == p) return false;
    size_t slen = (size_t)(sep - p);
    if (slen != 4 && slen != 5) return false;   /* http / https only */
    if (!ci_eq_n(p, "http", 4) && !(slen == 5 && ci_eq_n(p, "https", 5))) return false;
    p = sep + 3;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#' && *end != ':') end++;
    if (end == p) return false;
    /* Reject userinfo (user@host) — fail closed. */
    for (const char *q = p; q < end; ++q) if (*q == '@') return false;
    size_t n = (size_t)(end - p);
    if (n + 1 > cap) return false;
    memcpy(host, p, n);
    host[n] = 0;
    return true;
}

bool np_sb_host_match(const char *list, const char *host) {
    if (!list || !*list || !host || !*host) return false;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != ',' && *p != ';') p++;
        size_t n = (size_t)(p - s);
        if (!n) continue;
        if (n > 2 && s[0] == '*' && s[1] == '.') {
            /* "*.corp.local" must match a real subdomain, not the bare suffix:
             * compare the trailing ".corp.local" (with its leading dot). */
            const char *dot_suf = s + 1;          /* ".corp.local" */
            size_t dsl = n - 1;
            size_t hl = strlen(host);
            if (hl > dsl && ci_eq_n(host + hl - dsl, dot_suf, dsl)) return true;
        } else {
            if (strlen(host) == n && ci_eq_n(host, s, n)) return true;
        }
    }
    return false;
}

np_sb_verdict_t np_sb_check_url(const np_sb_policy_t *p, const char *url) {
    if (!p || !url || !*url) return NP_SB_DENY_ARG;
    char host[512];
    if (!np_sb_url_host(url, host, sizeof host)) return NP_SB_DENY_ARG;
    bool mirror = np_sb_host_match(p->mirror_hosts, host);
    if (p->offline_only) {
        /* Offline mode: deny unless explicitly on the mirror allow-list.
         * Empty mirror list = deny everything (fail-closed). */
        return mirror ? NP_SB_ALLOW : NP_SB_DENY_OFFLINE;
    }
    /* Online mode: still fail-closed on an explicit mirror list? No — online
     * mode allows public URLs; mirror_hosts is only meaningful offline. */
    return NP_SB_ALLOW;
}

/* ---- filesystem writes -------------------------------------------- */
np_sb_verdict_t np_sb_check_write(const np_sb_policy_t *p, const char *path) {
    if (!p || !path || !*path) return NP_SB_DENY_ARG;
    /* Traversal / device / absolute escapes are always denied. */
    if (strstr(path, "..")) return NP_SB_DENY_TRAVERSAL;
    if (path[0] == '/' || path[0] == '\\') return NP_SB_DENY_TRAVERSAL;
    if (path[1] == ':' && isalpha((unsigned char)path[0])) return NP_SB_DENY_TRAVERSAL;
    if (!p->write_allow[0]) return NP_SB_DENY_PATH;
    /* Allow-list entries are comma-separated directory prefixes. A write is
     * allowed only if its path starts with one of them (plus a separator). */
    const char *q = p->write_allow;
    while (*q) {
        while (*q == ' ' || *q == ',') q++;
        const char *s = q;
        while (*q && *q != ' ' && *q != ',') q++;
        size_t n = (size_t)(q - s);
        while (n > 0 && (s[n-1] == '/' || s[n-1] == '\\')) n--;  /* normalize trailing sep */
        if (!n) continue;
        if (strncmp(path, s, n) == 0 &&
            (path[n] == '/' || path[n] == '\\' || path[n] == 0)) return NP_SB_ALLOW;
    }
    return NP_SB_DENY_PATH;
}

/* ---- process / resources ------------------------------------------ */
np_sb_verdict_t np_sb_check_tool(const np_sb_policy_t *p, const char *exe) {
    if (!p || !exe || !*exe) return NP_SB_DENY_ARG;
    if (!p->allow_external_tools) return NP_SB_DENY_TOOL;
    /* Reject path separators: only a basename may be allow-listed. */
    if (strchr(exe, '/') || strchr(exe, '\\')) return NP_SB_DENY_TOOL;
    if (!p->tool_allow[0]) return NP_SB_DENY_TOOL;   /* empty = none allowed */
    const char *q = p->tool_allow;
    while (*q) {
        while (*q == ' ' || *q == ',') q++;
        const char *s = q;
        while (*q && *q != ' ' && *q != ',') q++;
        size_t n = (size_t)(q - s);
        if (n && strlen(exe) == n && ci_eq_n(exe, s, n)) return NP_SB_ALLOW;
    }
    return NP_SB_DENY_TOOL;
}

np_sb_verdict_t np_sb_check_resources(const np_sb_policy_t *p,
                                      uint32_t wall, uint32_t cpu, uint32_t mem) {
    if (!p) return NP_SB_DENY_ARG;
    if (p->max_wall_sec && wall > p->max_wall_sec) return NP_SB_DENY_RESOURCE;
    if (p->max_cpu_ms   && cpu  > p->max_cpu_ms)   return NP_SB_DENY_RESOURCE;
    if (p->max_mem_mb   && mem  > p->max_mem_mb)   return NP_SB_DENY_RESOURCE;
    return NP_SB_ALLOW;
}

bool np_sb_enforce(const np_sb_policy_t *p, np_sb_apply_fn apply,
                   void *process, void *user) {
    if (!p) return false;
    if (!apply) return true;   /* honest no-op: policy recorded, not enforced */
    return apply(p, process, user);
}

/* ---- audit --------------------------------------------------------- */
np_sb_audit_t *np_sb_audit_create(size_t cap) {
    if (!cap) cap = 64;
    np_sb_audit_t *a = (np_sb_audit_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->events = (np_sb_event_t *)calloc(cap, sizeof(np_sb_event_t));
    if (!a->events) { free(a); return NULL; }
    a->cap = cap;
    return a;
}
void np_sb_audit_destroy(np_sb_audit_t *a) {
    if (!a) return;
    free(a->events); free(a);
}
void np_sb_audit_set_clock(np_sb_audit_t *a, np_sb_clock_fn fn, void *user) {
    if (!a) return;
    a->clock = fn; a->clock_user = user;
}
void np_sb_audit_add(np_sb_audit_t *a, const char *action,
                     const char *target, np_sb_verdict_t v) {
    if (!a || !a->cap) return;
    np_sb_event_t *e = &a->events[a->head];
    memset(e, 0, sizeof *e);
    if (a->clock) a->clock(e->time_iso, sizeof e->time_iso, a->clock_user);
    scpy(e->action, sizeof e->action, action ? action : "");
    scpy(e->target, sizeof e->target, target ? target : "");
    e->verdict = v;
    a->head = (a->head + 1) % a->cap;
    if (a->count < a->cap) a->count++;
}
size_t np_sb_audit_count(const np_sb_audit_t *a) { return a ? a->count : 0; }
const np_sb_event_t *np_sb_audit_at(const np_sb_audit_t *a, size_t i) {
    if (!a || i >= a->count) return NULL;
    /* Oldest event sits at (head - count + cap) % cap. */
    size_t start = (a->head + a->cap - a->count) % a->cap;
    return &a->events[(start + i) % a->cap];
}

/* ---- persistence --------------------------------------------------- */
bool np_sb_save(const np_sb_policy_t *p, const char *path) {
    if (!p || !path) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "offline_only=%d\n", p->offline_only ? 1 : 0);
    fprintf(f, "mirror_hosts=%s\n", p->mirror_hosts);
    fprintf(f, "write_allow=%s\n", p->write_allow);
    fprintf(f, "allow_external_tools=%d\n", p->allow_external_tools ? 1 : 0);
    fprintf(f, "tool_allow=%s\n", p->tool_allow);
    fprintf(f, "max_wall_sec=%u\n", p->max_wall_sec);
    fprintf(f, "max_cpu_ms=%u\n", p->max_cpu_ms);
    fprintf(f, "max_mem_mb=%u\n", p->max_mem_mb);
    return fclose(f) == 0;
}

size_t np_sb_load(np_sb_policy_t *p, const char *path) {
    if (!p || !path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[NP_SB_LIST_CAP + 64];
    size_t keys = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "offline_only")) p->offline_only = atoi(v) != 0;
        else if (!strcmp(line, "mirror_hosts")) scpy(p->mirror_hosts, sizeof p->mirror_hosts, v);
        else if (!strcmp(line, "write_allow")) scpy(p->write_allow, sizeof p->write_allow, v);
        else if (!strcmp(line, "allow_external_tools")) p->allow_external_tools = atoi(v) != 0;
        else if (!strcmp(line, "tool_allow")) scpy(p->tool_allow, sizeof p->tool_allow, v);
        else if (!strcmp(line, "max_wall_sec")) p->max_wall_sec = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(line, "max_cpu_ms")) p->max_cpu_ms = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(line, "max_mem_mb")) p->max_mem_mb = (uint32_t)strtoul(v, NULL, 10);
        else continue;
        keys++;
    }
    fclose(f);
    return keys;
}
