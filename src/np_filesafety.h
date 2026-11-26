#ifndef NP_FILESAFETY_H
#define NP_FILESAFETY_H

/*
 * np_filesafety.h - H8: sensitive-file safety boundary for the binary track.
 *
 * Executables, keys, certificates and signature files must not be written by
 * AI by default. Decision is based on BOTH extension and magic bytes; a file
 * is writable only if (a) it is not on the deny list and (b) the user has
 * explicitly enabled writing for it in project settings. Every write goes
 * into an audit record.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    NP_FS_WRITE_DENIED = 0,   /* default: do not write                     */
    NP_FS_WRITE_ALLOWED,      /* explicitly enabled by user                */
    NP_FS_EXPLAIN_ONLY        /* may explain, never write (always)         */
} np_fs_decision_t;

const char *np_fs_decision_name(np_fs_decision_t d);

/* Decide write policy from extension + content magic.
 *   path      : file path (may be NULL; extension check skipped)
 *   data,len  : leading bytes for magic sniffing (may be NULL)
 *   user_enabled: explicit per-file/project opt-in from settings
 * Explain-only formats (keys/certs) are NEVER writable regardless of opt-in. */
np_fs_decision_t np_fs_decide(const char *path, const void *data, size_t len,
                              bool user_enabled);

/* Reason string for the last decision category of a file (for the UI). */
const char *np_fs_reason(const char *path, const void *data, size_t len);

/* Audit record append (in-memory ring; the app persists it). */
typedef struct {
    char     file[260];
    char     action[32];   /* "write" / "explain" / "denied"             */
    char     tool[80];
    uint64_t revision;
} np_audit_record_t;

typedef struct np_audit_log np_audit_log_t;
np_audit_log_t *np_audit_create(size_t capacity);
void np_audit_destroy(np_audit_log_t *log);
bool np_audit_append(np_audit_log_t *log, const np_audit_record_t *rec);
size_t np_audit_count(const np_audit_log_t *log);
const np_audit_record_t *np_audit_at(const np_audit_log_t *log, size_t i);

#ifdef __cplusplus
}
#endif
#endif /* NP_FILESAFETY_H */
