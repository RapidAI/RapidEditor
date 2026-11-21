/* Unit tests for the maclaw protocol layer's JSON parsing & normalization. */
#include "np_maclaw.h"
#include "np_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

/* Stub transport so the protocol layer can be tested without a Hub. */
static char g_url[1200], g_bearer[512], g_body[65536];
static const char *g_response;
static int g_status;

char *np_net_post_json(const char *url, const char *bearer, const char *body,
                       const np_net_opts *o, int *status_out) {
    (void)o;
    snprintf(g_url, sizeof g_url, "%s", url ? url : "");
    snprintf(g_bearer, sizeof g_bearer, "%s", bearer ? bearer : "");
    snprintf(g_body, sizeof g_body, "%s", body ? body : "");
    if (status_out) *status_out = g_status;
    if (!g_response) return NULL;
    size_t n = strlen(g_response) + 1;
    char *copy = (char *)malloc(n);
    if (!copy) return NULL;
    memcpy(copy, g_response, n);
    return copy;
}

char *np_net_get_json(const char *url, const char *bearer,
                      const np_net_opts *o, int *status_out) {
    (void)o; (void)bearer;
    snprintf(g_url, sizeof g_url, "%s", url ? url : "");
    if (status_out) *status_out = g_status;
    /* Test hook: hosts with "down" always fail their probe. */
    if (url && strstr(url, "down")) return NULL;
    return strdup("ok");
}

int main(void) {
    char out[2048];

    /* Plain string field. */
    CHECK(np_json_get_string("{\"status\":\"pending\",\"poll_token\":\"abc\"}",
                             "status", out, sizeof out) && !strcmp(out, "pending"),
          "parse plain string");
    CHECK(np_json_get_string("{\"status\":\"pending\",\"poll_token\":\"abc\"}",
                             "poll_token", out, sizeof out) && !strcmp(out, "abc"),
          "parse second string");

    /* Escaped JSON string (qr_payload is itself a JSON document). */
    const char *resp =
        "{\"status\":\"pending\",\"qr_payload\":\"{\\\"v\\\":2,\\\"type\\\":\\\"maclaw_third_party_authorization\\\",\\\"session_id\\\":\\\"maqr_x\\\"}\"}";
    CHECK(np_json_get_string(resp, "qr_payload", out, sizeof out),
          "parse escaped qr_payload");
    CHECK(strstr(out, "\"type\":\"maclaw_third_party_authorization\"") != NULL,
          "qr_payload unescaped correctly");
    CHECK(strstr(out, "\\\"") == NULL, "no leftover backslash escapes");

    /* Unicode escape -> UTF-8 (Chinese 测 = U+6D4B). */
    CHECK(np_json_get_string("{\"name\":\"\\u6d4b\\u8bd5\"}", "name", out, sizeof out),
          "parse unicode escape");
    CHECK((unsigned char)out[0] == 0xE6 && (unsigned char)out[1] == 0xB5 &&
          (unsigned char)out[2] == 0x8B, "unicode -> UTF-8 bytes");

    /* Missing key. */
    out[0] = 'X';
    CHECK(!np_json_get_string("{\"a\":\"b\"}", "missing", out, sizeof out) && out[0] == 0,
          "missing key clears output");

    /* Hub URL normalization. */
    np_maclaw_normalize_hub(" https://hub.maclaw.top/  ", out, sizeof out);
    CHECK(!strcmp(out, "https://hub.maclaw.top"), "normalize https + slash + spaces");
    np_maclaw_normalize_hub("hub.example.com", out, sizeof out);
    CHECK(!strcmp(out, "https://hub.example.com"), "add https scheme");
    np_maclaw_normalize_hub("http://localhost:8080/", out, sizeof out);
    CHECK(!strcmp(out, "http://localhost:8080"), "preserve http + port");

    /* HubCenter HA probing: first healthy entry wins. */
    {
        char picked[512] = {0}, perr[256] = {0};
        CHECK(np_maclaw_pick_center("https://a.example.com, https://b.example.com",
                                    picked, sizeof picked, perr, sizeof perr) &&
              !strcmp(picked, "https://a.example.com"),
              "pick center first healthy");
        CHECK(strstr(g_url, "https://a.example.com/healthz") != NULL,
              "probe hits healthz");
        /* First node down -> falls through to the second. */
        CHECK(np_maclaw_pick_center("https://down.example.com,https://b.example.com,https://c.example.com",
                                    picked, sizeof picked, perr, sizeof perr) &&
              !strcmp(picked, "https://b.example.com"),
              "pick center skips down node");
        CHECK(strstr(g_url, "https://b.example.com/healthz") != NULL,
              "second node probed after failure");
        /* All nodes down -> best-effort fallback to the first entry. */
        CHECK(!np_maclaw_pick_center("https://down1.example.com,https://down2.example.com",
                                     picked, sizeof picked, perr, sizeof perr) &&
              !strcmp(picked, "https://down1.example.com"),
              "pick center falls back to first");
    }

    /* Create session: posts to the HubCenter relay endpoint. */
    np_maclaw_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.center_url, sizeof cfg.center_url, "%s", "hubs.mypapers.top/");
    np_maclaw_normalize_hub(cfg.center_url, cfg.center_url, sizeof cfg.center_url);
    snprintf(cfg.client_id, sizeof cfg.client_id, "%s", "rapideditor-ai-assistant");
    snprintf(cfg.client_name, sizeof cfg.client_name, "%s", "RapidEditor AI");
    cfg.timeout_sec = 10;
    np_maclaw_session sess;
    memset(&sess, 0, sizeof sess);
    g_status = 201;
    g_response = "{\"status\":\"pending\",\"session_id\":\"tpr_s1\","
                 "\"poll_token\":\"pt1\","
                 "\"qr_payload\":\"{\\\"type\\\":\\\"maclaw_third_party_authorization\\\",\\\"session_id\\\":\\\"tpr_s1\\\",\\\"center_url\\\":\\\"https://hubs.mypapers.top\\\"}\","
                 "\"expires_at\":\"2026-11-21T00:00:00Z\"}";
    CHECK(np_maclaw_create_session(&cfg, &sess), "create relay session");
    CHECK(strstr(g_url, "https://hubs.mypapers.top/api/third-party/sessions") != NULL,
          "create posts to hubcenter relay endpoint");
    CHECK(strstr(g_body, "\"client_id\":\"rapideditor-ai-assistant\"") != NULL,
          "create body carries client_id");
    CHECK(!strcmp(sess.session_id, "tpr_s1") && !strcmp(sess.poll_token, "pt1"),
          "create parses session id and poll token");
    CHECK(strstr(sess.qr_payload, "\"center_url\":\"https://hubs.mypapers.top\"") != NULL,
          "qr payload carries center_url");
    CHECK(strstr(sess.qr_payload, "hub_url") == NULL,
          "qr payload must not carry a hub_url");

    /* Poll: pending then authorized with the relayed hub_url. */
    g_status = 200;
    g_response = "{\"status\":\"pending\"}";
    char tok[512] = {0}, hub[512] = {0}, email[256] = {0}, perr[256] = {0};
    CHECK(np_maclaw_poll(&cfg, &sess, tok, sizeof tok, hub, sizeof hub,
                         email, sizeof email, perr, sizeof perr) &&
          !strcmp(np_maclaw_poll(&cfg, &sess, tok, sizeof tok, hub, sizeof hub,
                                 email, sizeof email, perr, sizeof perr),
                  "pending"),
          "poll pending");
    CHECK(strstr(g_url, "https://hubs.mypapers.top/api/third-party/sessions/token") != NULL,
          "poll posts to hubcenter token endpoint");
    CHECK(strstr(g_body, "\"poll_token\":\"pt1\"") != NULL, "poll sends poll_token");

    g_response = "{\"status\":\"authorized\",\"access_token\":\"vtok_9\","
                 "\"hub_url\":\"https://hub.maclaw.top\",\"email\":\"u@x.com\"}";
    tok[0] = hub[0] = email[0] = 0;
    const char *pst = np_maclaw_poll(&cfg, &sess, tok, sizeof tok, hub, sizeof hub,
                                     email, sizeof email, perr, sizeof perr);
    CHECK(pst && !strcmp(pst, "authorized"), "poll authorized");
    CHECK(!strcmp(tok, "vtok_9"), "poll parses access_token");
    CHECK(!strcmp(hub, "https://hub.maclaw.top"), "poll parses relayed hub_url");
    CHECK(!strcmp(email, "u@x.com"), "poll parses email");

    /* Official LLM proxy: URL, bearer, request body and response content. */
    g_status = 200;
    g_response = "{\"choices\":[{\"message\":{\"role\":\"assistant\","
                 "\"content\":\"line1\\nline2\"}}]}";
    char answer[1024] = {0}, err[256] = {0};
    CHECK(np_maclaw_llm_chat("hub.example.com/", "tok-1",
                             "say \"hi\"\nnext", answer, sizeof answer,
                             err, sizeof err),
          "official LLM chat succeeds");
    CHECK(!strcmp(answer, "line1\nline2"), "official LLM response unescaped");
    CHECK(!strcmp(g_bearer, "tok-1"), "official LLM sends viewer token");
    CHECK(strstr(g_url, "https://hub.example.com/api/llm/v1/chat/completions") != NULL,
          "official LLM posts to Hub LLM endpoint");
    CHECK(strstr(g_body, "\"model\":\"auto\"") != NULL,
          "official LLM uses auto model");
    CHECK(strstr(g_body, "say \\\"hi\\\"\\nnext") != NULL,
          "official LLM escapes prompt");

    g_response = NULL;
    g_status = 401;
    answer[0] = 'X'; err[0] = 0;
    CHECK(!np_maclaw_llm_chat("hub.example.com", "tok-1", "hello",
                              answer, sizeof answer, err, sizeof err),
          "official LLM transport failure reported");
    CHECK(answer[0] == 0 && err[0] != 0, "official LLM failure clears output");

    printf("%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
