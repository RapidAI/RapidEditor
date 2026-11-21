#ifndef NP_MACLAW_H
#define NP_MACLAW_H

/* maclaw official-service "scan to authorize" client for the RapidEditor AI
 * assistant. The desktop client does NOT know which Hub holds the user's
 * account — only the maclaw HubCenter (the directory service) does. The flow:
 *
 *   1. POST <center>/api/third-party/sessions        (create, no auth)
 *      -> qr_payload carries ONLY center_url + session_id, never a hub_url
 *   2. A signed-in maclawbot phone scans the QR, authorizes on its OWN
 *      account's Hub, and delivers {hub_url, access_token, email} back to
 *      the HubCenter mailbox.
 *   3. POST <center>/api/third-party/sessions/token   (poll, no auth)
 *      -> one-time {hub_url, access_token, email}; the hub_url is resolved
 *         by the relay and must be kept for the official LLM endpoint:
 *            Authorization: Bearer <token> -> <hub>/api/llm/v1/chat/completions
 *         (billed to that maclaw account's own credits).
 *
 * All calls are synchronous and intended to run on a worker thread. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char center_url[512];    /* HubCenter base, e.g. https://hubs.mypapers.top */
    char client_id[128];     /* stable per-install id */
    char client_name[128];   /* display name shown on the phone */
    int  timeout_sec;
} np_maclaw_cfg;

typedef struct {
    char session_id[128];
    char poll_token[128];
    char qr_payload[2048];   /* the JSON string to encode as a QR */
    char expires_at[64];
    char err[256];
} np_maclaw_session;

/* POST create-session against the HubCenter. Returns true and fills `s` on
 * 201. */
bool np_maclaw_create_session(const np_maclaw_cfg *cfg, np_maclaw_session *s);

/* Poll credential status. Returns one of:
 *   "pending"    -> not authorized yet (call again)
 *   "authorized" -> token delivered in `token`, relayed hub in `hub_url`
 *   "delivered"  -> token already picked up
 *   "expired"    -> session gone/expired
 *   NULL         -> transport/parse error (see err)
 * On "authorized", fills `token`, `hub_url` and `email`. */
const char *np_maclaw_poll(const np_maclaw_cfg *cfg, const np_maclaw_session *s,
                           char *token, size_t token_cap,
                           char *hub_url, size_t hub_url_cap,
                           char *email, size_t email_cap,
                           char *err, size_t err_cap);

/* Send one non-streaming chat completion through the Hub's official LLM
 * proxy using the viewer_token from np_maclaw_poll(). Returns true and writes
 * choices[0].message.content to `out`; errors go to `err`. */
bool np_maclaw_llm_chat(const char *hub_url, const char *token,
                        const char *prompt, char *out, size_t out_cap,
                        char *err, size_t err_cap);

/* Normalize a user-entered URL (trim trailing slash, ensure scheme). Safe
 * for in-place calls (in == out). */
void np_maclaw_normalize_hub(const char *in, char *out, size_t cap);

/* Probe a comma-separated list of HubCenter base URLs and pick the first
 * reachable one (GET /healthz must return 2xx). On total failure the first
 * entry is still written to `out` (best effort) and false is returned. */
bool np_maclaw_pick_center(const char *centers_csv, char *out, size_t cap,
                           char *err, size_t err_cap);

/* Extract a string field from a JSON object into `out` (handles escapes).
 * Returns true if found. */
bool np_json_get_string(const char *json, const char *key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_MACLAW_H */
