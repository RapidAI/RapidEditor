#ifndef NP_MACLAW_H
#define NP_MACLAW_H

/* maclaw official-service "scan to authorize" client for the RapidEditor AI
 * assistant. Implements the third-party device-authorization flow against a
 * MaClaw Hub using the endpoints added in hub/internal/httpapi:
 *   POST /api/mobile/auth/third-party-qr-sessions        (create, no auth)
 *   POST /api/mobile/auth/third-party-qr-sessions/token  (poll, no auth)
 * The QR is scanned by a signed-in maclawbot phone, which authorizes the
 * session; polling then yields a viewer_token usable as
 *   Authorization: Bearer <token>  ->  /api/llm/v1/chat/completions
 * (billed to that maclaw account's own credits).
 *
 * All calls are synchronous and intended to run on a worker thread. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char hub_url[512];       /* normalized base, e.g. https://hub.maclaw.top */
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

/* POST create-session. Returns true and fills `s` on 201. */
bool np_maclaw_create_session(const np_maclaw_cfg *cfg, np_maclaw_session *s);

/* Poll token status. Returns one of:
 *   "pending"    -> not authorized yet (call again)
 *   "authorized" -> token delivered in `token` (one-time)
 *   "delivered"  -> token already picked up
 *   "expired"    -> session gone/expired
 *   NULL         -> transport/parse error (see err)
 * On "authorized", fills `token` and `email`. */
const char *np_maclaw_poll(const np_maclaw_cfg *cfg, const np_maclaw_session *s,
                           char *token, size_t token_cap,
                           char *email, size_t email_cap,
                           char *err, size_t err_cap);

/* Send one non-streaming chat completion through the Hub's official LLM
 * proxy using the viewer_token from np_maclaw_poll(). Returns true and writes
 * choices[0].message.content to `out`; errors go to `err`. */
bool np_maclaw_llm_chat(const char *hub_url, const char *token,
                        const char *prompt, char *out, size_t out_cap,
                        char *err, size_t err_cap);

/* Normalize a user-entered hub URL (trim trailing slash, ensure scheme). */
void np_maclaw_normalize_hub(const char *in, char *out, size_t cap);

/* Extract a string field from a JSON object into `out` (handles escapes).
 * Returns true if found. */
bool np_json_get_string(const char *json, const char *key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_MACLAW_H */
