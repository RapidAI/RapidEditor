#ifndef NP_OOXML_WRITE_H
#define NP_OOXML_WRITE_H

/*
 * np_ooxml_write.h - F8: safe OOXML write-back and re-packaging.
 *
 * Editing container bytes or DB pages directly corrupts files. This module:
 *   1. reads every part of an OPC package (STORE/DEFLATE, self-contained);
 *   2. replaces ONE XML part with validated content (well-formedness gate);
 *   3. re-packages with STORE entries (method 0, CRC32 per part) — a valid
 *      ZIP/OPC that any reader opens;
 *   4. RE-OPENS the result (open-read-back) to prove it is parseable before
 *      the caller commits it;
 *   5. keeps a one-step Ctrl+Z snapshot: restore() atomically puts back the
 *      original bytes.
 *
 * Fail-closed: nothing is written to the final path until every gate passes;
 * the write goes to a temp file then an atomic rename. UI-free C99.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define NP_OXW_PART_CAP    512
#define NP_OXW_NAME_CAP    512
#define NP_OXW_MAX_PARTS   256
#define NP_OXW_MAX_PART_B  (32u * 1024 * 1024)

typedef struct {
    char           name[NP_OXW_NAME_CAP];  /* OPC part path, e.g. word/document.xml */
    unsigned char *data;
    size_t         len;
} np_oxw_part_t;

typedef struct {
    np_oxw_part_t parts[NP_OXW_MAX_PARTS];
    size_t        count;
    char          err[256];
} np_oxw_pkg_t;

/* Read all parts from a ZIP/OPC file into memory. Returns true on success. */
bool np_oxw_read(const char *zip_path, np_oxw_pkg_t *pkg);
void np_oxw_free(np_oxw_pkg_t *pkg);

/* Find a part by exact path. */
np_oxw_part_t *np_oxw_find(np_oxw_pkg_t *pkg, const char *name);
const np_oxw_part_t *np_oxw_find_c(const np_oxw_pkg_t *pkg, const char *name);

/* XML well-formedness gate: tag pairing, unclosed tags, illegal control chars.
 * Not a full XML parser — it is the safety check before write-back. */
bool np_oxw_xml_wellformed(const char *xml, size_t len, char *err, size_t err_cap);

/* Replace one part's content (copies bytes). Part must exist. */
bool np_oxw_set_part(np_oxw_pkg_t *pkg, const char *name,
                     const unsigned char *data, size_t len);

/* Repackage all parts with STORE entries into `out_path` (temp+rename).
 * Then open-read-back using the caller-injected verifier (e.g. np_ooxml
 * parse): `verify` receives the written path; return false to abort. If
 * `backup_path` is non-NULL, the original file bytes are copied there first
 * for one-step Ctrl+Z. Returns false and leaves no final file on any failure. */
typedef bool (*np_oxw_verify_fn)(const char *path, void *user);
bool np_oxw_commit(np_oxw_pkg_t *pkg, const char *out_path,
                   const char *original_path, const char *backup_path,
                   np_oxw_verify_fn verify, void *user,
                   char *err, size_t err_cap);

/* Ctrl+Z: restore `backup_path` over `target_path` atomically. */
bool np_oxw_restore(const char *backup_path, const char *target_path,
                    char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_OOXML_WRITE_H */
