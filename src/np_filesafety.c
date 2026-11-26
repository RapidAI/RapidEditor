/* np_filesafety.c - H8 sensitive-file boundary (see np_filesafety.h). */
#include "np_filesafety.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

const char *np_fs_decision_name(np_fs_decision_t d) {
    switch (d) {
        case NP_FS_WRITE_DENIED:  return "denied";
        case NP_FS_WRITE_ALLOWED: return "allowed";
        case NP_FS_EXPLAIN_ONLY:  return "explain-only";
        default:                  return "unknown";
    }
}

/* extensions that are NEVER writable (keys/certs/signatures) */
static const char *kExplainOnlyExt[] = {
    ".key", ".pem", ".cer", ".crt", ".der", ".pfx", ".p12", ".jks",
    ".keystore", ".sig", ".asc", ".gpg", ".pgp", "id_rsa", "id_ed25519"
};
/* executable / binary formats: denied unless explicitly enabled */
static const char *kDenyExt[] = {
    ".exe", ".dll", ".so", ".dylib", ".o", ".obj", ".a", ".lib",
    ".bin", ".elf", ".sys", ".drv", ".msi", ".apk", ".wasm", ".pyc", ".class"
};
/* magic prefixes -> explain-only (credentials) or denied (binaries) */
typedef struct { const unsigned char *sig; size_t len; bool explain_only; const char *name; } magic_t;
static const unsigned char kElf[4] = {0x7F,'E','L','F'};
static const unsigned char kPE[2]  = {'M','Z'};
/* PEM text keys are detected via "-----BEGIN ... KEY/CERTIFICATE" below. */
static const unsigned char kPKCS8[]= {0x30,0x82};
static const magic_t kMagic[] = {
    { kElf, 4, false, "ELF executable/object" },
    { kPE,  2, false, "PE executable" },
    { kPKCS8,2, true, "DER certificate/key" }
};

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool ext_matches(const char *path, const char *ext) {
    size_t pl = strlen(path), el = strlen(ext);
    if (el > pl) return false;
    /* match at end, case-insensitive, no separator requirement for names like id_rsa */
    const char *tail = path + pl - el;
    for (size_t i = 0; i < el; ++i)
        if (lower(tail[i]) != lower(ext[i])) return false;
    return true;
}

np_fs_decision_t np_fs_decide(const char *path, const void *data, size_t len,
                              bool user_enabled) {
    /* PEM private keys / certs are textual but explain-only. */
    if (path) {
        for (size_t i = 0; i < sizeof(kExplainOnlyExt)/sizeof(kExplainOnlyExt[0]); ++i)
            if (ext_matches(path, kExplainOnlyExt[i])) return NP_FS_EXPLAIN_ONLY;
    }
    if (data && len >= 26) {
        const char *s = (const char *)data;
        if (memcmp(s, "-----BEGIN ", 11) == 0 &&
            (strstr(s, "PRIVATE KEY") || strstr(s, "CERTIFICATE")))
            return NP_FS_EXPLAIN_ONLY;
    }
    if (data && len) {
        for (size_t i = 0; i < sizeof(kMagic)/sizeof(kMagic[0]); ++i) {
            const magic_t *m = &kMagic[i];
            if (len >= m->len && memcmp(data, m->sig, m->len) == 0) {
                /* credentials are never writable; executables follow opt-in */
                if (m->explain_only) return NP_FS_EXPLAIN_ONLY;
                return user_enabled ? NP_FS_WRITE_ALLOWED : NP_FS_WRITE_DENIED;
            }
        }
    }
    if (path) {
        for (size_t i = 0; i < sizeof(kDenyExt)/sizeof(kDenyExt[0]); ++i)
            if (ext_matches(path, kDenyExt[i]))
                return user_enabled ? NP_FS_WRITE_ALLOWED : NP_FS_WRITE_DENIED;
    }
    /* ordinary data file: still requires explicit opt-in to write */
    return user_enabled ? NP_FS_WRITE_ALLOWED : NP_FS_WRITE_DENIED;
}

const char *np_fs_reason(const char *path, const void *data, size_t len) {
    np_fs_decision_t d = np_fs_decide(path, data, len, false);
    switch (d) {
        case NP_FS_EXPLAIN_ONLY: return "密钥/证书/签名文件：仅解释，永不写入";
        case NP_FS_WRITE_DENIED: return "可执行/二进制或数据文件：默认禁写，需项目设置显式开启";
        default:                 return "允许写入（已显式开启）";
    }
}

/* ---------------- audit log ---------------- */
struct np_audit_log {
    np_audit_record_t *recs;
    size_t cap, count, head;
};

np_audit_log_t *np_audit_create(size_t capacity) {
    np_audit_log_t *l = (np_audit_log_t *)calloc(1, sizeof(*l));
    if (!l) return NULL;
    if (capacity == 0) capacity = 128;
    l->recs = (np_audit_record_t *)calloc(capacity, sizeof(*l->recs));
    if (!l->recs) { free(l); return NULL; }
    l->cap = capacity;
    return l;
}

void np_audit_destroy(np_audit_log_t *log) {
    if (!log) return;
    free(log->recs);
    free(log);
}

bool np_audit_append(np_audit_log_t *log, const np_audit_record_t *rec) {
    if (!log || !rec) return false;
    size_t idx = (log->head + log->count) % log->cap;
    log->recs[idx] = *rec;
    if (log->count < log->cap) log->count++;
    else log->head = (log->head + 1) % log->cap;
    return true;
}

size_t np_audit_count(const np_audit_log_t *log) { return log ? log->count : 0; }

const np_audit_record_t *np_audit_at(const np_audit_log_t *log, size_t i) {
    if (!log || i >= log->count) return NULL;
    return &log->recs[(log->head + i) % log->cap];
}
