#ifndef NP_SANDBOX_H
#define NP_SANDBOX_H

/*
 * np_sandbox.h - T10: analysis sandbox policy gate + enterprise offline mode.
 *
 * Malware samples, malformed files and auto-downloaded tools must not touch the
 * host environment unchecked. This module is the POLICY DECISION layer:
 * fail-closed checks for URLs (offline-only / mirror allow-list), filesystem
 * writes (allow-list + traversal rejection), external tool execution and
 * resource ceilings. Resource ENFORCEMENT on Windows uses a Job Object handle
 * supplied by the caller; on POSIX the limits are recorded honestly as
 * "not enforced by this module" (caller must use setrlimit/seccomp).
 *
 * UI-free, no process spawning of its own, no OS calls except an optional
 * injected Job Object apply function. C99.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define NP_SB_URL_CAP      512
#define NP_SB_LIST_CAP     2048
#define NP_SB_PATH_CAP     512
#define NP_SB_MIRROR_MAX   16

typedef enum {
    NP_SB_ALLOW = 0,
    NP_SB_DENY_OFFLINE,
    NP_SB_DENY_DOMAIN,
    NP_SB_DENY_PATH,
    NP_SB_DENY_TRAVERSAL,
    NP_SB_DENY_TOOL,
    NP_SB_DENY_RESOURCE,
    NP_SB_DENY_ARG
} np_sb_verdict_t;

const char *np_sb_verdict_name(np_sb_verdict_t v);

typedef struct {
    /* Enterprise offline policy: when true, PUBLIC network URLs are denied;
     * only URLs whose host matches a configured mirror are allowed. */
    bool   offline_only;
    /* Comma-separated mirror host allow-list (e.g. "mirror.corp.local,*.corp"). */
    char   mirror_hosts[NP_SB_LIST_CAP];
    /* Comma-separated filesystem write allow-list (absolute or repo-relative
     * directories). Empty = no external writes allowed. */
    char   write_allow[NP_SB_LIST_CAP];
    /* Whether external analysis tools may be executed at all. */
    bool   allow_external_tools;
    /* Comma-separated executable basenames allowed to launch. */
    char   tool_allow[NP_SB_LIST_CAP];
    /* Resource ceilings. 0 = unspecified (caller decides). */
    uint32_t max_wall_sec;
    uint32_t max_cpu_ms;
    uint32_t max_mem_mb;
} np_sb_policy_t;

void np_sb_policy_defaults(np_sb_policy_t *p);

/* Parse a scheme://host/path URL, returning host into `host`. Honest: rejects
 * malformed / userinfo / port-only tricks (fail-closed). */
bool np_sb_url_host(const char *url, char *host, size_t cap);

/* Domain match with "*.suffix" wildcard. Bare suffix does NOT match its own
 * parent (must be a real subdomain), same rule as X9. */
bool np_sb_host_match(const char *list, const char *host);

/* Policy decisions — all fail-closed on malformed input. */
np_sb_verdict_t np_sb_check_url(const np_sb_policy_t *p, const char *url);
np_sb_verdict_t np_sb_check_write(const np_sb_policy_t *p, const char *path);
np_sb_verdict_t np_sb_check_tool(const np_sb_policy_t *p, const char *exe_basename);
np_sb_verdict_t np_sb_check_resources(const np_sb_policy_t *p,
                                      uint32_t wall_sec, uint32_t cpu_ms, uint32_t mem_mb);

/* Resource enforcement adapter: caller injects the OS-specific handler (e.g.
 * assign process to a Windows Job Object with limits). Returns true on
 * success. `user` carries the process handle/context. If `apply` is NULL the
 * policy is recorded but NOT enforced (honest no-op, returns true). */
typedef bool (*np_sb_apply_fn)(const np_sb_policy_t *p, void *process, void *user);
bool np_sb_enforce(const np_sb_policy_t *p, np_sb_apply_fn apply,
                   void *process, void *user);

/* Audit event: one line appended to an in-memory ring (bounded). */
typedef struct {
    char     time_iso[32];     /* filled by caller via np_sb_audit_set_time */
    char     action[32];       /* "url" / "write" / "tool" / "fetch" */
    char     target[NP_SB_PATH_CAP];
    np_sb_verdict_t verdict;
} np_sb_event_t;

typedef struct np_sb_audit np_sb_audit_t;
np_sb_audit_t *np_sb_audit_create(size_t cap);
void np_sb_audit_destroy(np_sb_audit_t *a);
/* Optional clock injector: fmt into buf (e.g. UTC ISO-8601). NULL = empty. */
typedef void (*np_sb_clock_fn)(char *buf, size_t cap, void *user);
void np_sb_audit_set_clock(np_sb_audit_t *a, np_sb_clock_fn fn, void *user);
void np_sb_audit_add(np_sb_audit_t *a, const char *action,
                     const char *target, np_sb_verdict_t v);
size_t np_sb_audit_count(const np_sb_audit_t *a);
const np_sb_event_t *np_sb_audit_at(const np_sb_audit_t *a, size_t i);

/* Persistence: sandbox.ini style key=value. Only policy fields are stored. */
bool np_sb_save(const np_sb_policy_t *p, const char *path);
size_t np_sb_load(np_sb_policy_t *p, const char *path);   /* returns #keys loaded */

#ifdef __cplusplus
}
#endif
#endif /* NP_SANDBOX_H */
