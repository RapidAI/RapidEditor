/* Unit tests for the OpenAI-compatible provider module (stub transport). */
#include "np_openai.h"
#include "np_net.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static char g_url[1200], g_bearer[512], g_body[65536], g_method[16];
static const char *g_response;
static int g_status;
/* Streaming stub state: when g_sse is set, feed it to the chunk callback. */
static const char *g_sse;
static int g_sse_chunks;
static volatile bool g_stub_cancel;
static int delta_calls;

char *np_net_post_json(const char *url, const char *bearer, const char *body,
                       const np_net_opts *o, int *status_out) {
    (void)o;
    snprintf(g_method, sizeof g_method, "POST");
    snprintf(g_url, sizeof g_url, "%s", url ? url : "");
    snprintf(g_bearer, sizeof g_bearer, "%s", bearer ? bearer : "");
    snprintf(g_body, sizeof g_body, "%s", body ? body : "");
    if (status_out) *status_out = g_status;
    if (!g_response) return NULL;
    return strdup(g_response);
}

/* Streaming transport stub: feed a scripted SSE payload in two chunks so the
 * decoder's line-splitting across chunk boundaries is exercised too. */
bool np_net_post_json_stream(const char *url, const char *bearer, const char *body,
                             const np_net_opts *o, int *status_out,
                             bool (*chunk_cb)(const char *, size_t, void *), void *user) {
    (void)o;
    snprintf(g_method, sizeof g_method, "STREAM");
    snprintf(g_url, sizeof g_url, "%s", url ? url : "");
    snprintf(g_bearer, sizeof g_bearer, "%s", bearer ? bearer : "");
    snprintf(g_body, sizeof g_body, "%s", body ? body : "");
    if (status_out) *status_out = g_status;
    if (!g_sse) return false;
    size_t n = strlen(g_sse), mid = n / 2;
    /* split at a newline when possible, else raw midpoint */
    size_t cut = mid;
    while (cut > 0 && cut < n && g_sse[cut] != '\n') cut++;
    if (cut >= n) cut = mid;
    if (!chunk_cb(g_sse, cut, user)) return false;
    g_sse_chunks = 1;
    if (g_stub_cancel) return false;
    if (!chunk_cb(g_sse + cut, n - cut, user)) return false;
    g_sse_chunks = 2;
    return true;
}

static bool delta_probe(const char *delta, size_t len, void *user) {
    (void)delta; (void)len; (void)user;
    delta_calls++;
    return true;
}

/* Cancel after the first delta to verify partial output is preserved. */
static bool delta_probe_cancel(const char *delta, size_t len, void *user) {
    (void)delta; (void)len; (void)user;
    delta_calls++;
    return delta_calls < 2;
}

char *np_net_get_json(const char *url, const char *bearer,
                      const np_net_opts *o, int *status_out) {
    (void)o;
    snprintf(g_method, sizeof g_method, "GET");
    snprintf(g_url, sizeof g_url, "%s", url ? url : "");
    snprintf(g_bearer, sizeof g_bearer, "%s", bearer ? bearer : "");
    if (status_out) *status_out = g_status;
    if (!g_response) return NULL;
    return strdup(g_response);
}

int main(void) {
    char out[2048], err[256];

    /* Chat: URL normalization (strips /v1, adds scheme), key, body, content. */
    g_status = 200;
    g_response = "{\"choices\":[{\"message\":{\"role\":\"assistant\","
                 "\"content\":\"你好\\nworld\"}}]}";
    CHECK(np_openai_chat("api.example.com/v1/", "sk-1", "deepseek-chat",
                         "say \"hi\"", out, sizeof out, err, sizeof err),
          "chat succeeds");
    CHECK(!strcmp(out, "你好\nworld"), "chat content unescaped");
    CHECK(!strcmp(g_method, "POST"), "chat uses POST");
    CHECK(!strcmp(g_url, "https://api.example.com/v1/chat/completions"),
          "chat normalizes base and appends endpoint");
    CHECK(!strcmp(g_bearer, "sk-1"), "chat sends api key as bearer");
    CHECK(strstr(g_body, "\"model\":\"deepseek-chat\"") != NULL, "chat carries model");
    CHECK(strstr(g_body, "say \\\"hi\\\"") != NULL, "chat escapes prompt");

    /* chat_ex: per-call sampling params land in the body; zero temperature
     * omits the field (provider default) like the global-param convention. */
    g_status = 200;
    g_response = "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
    CHECK(np_openai_chat_ex("https://api.example.com", "sk-1", "m", "hi",
                            0.f, 400, 30, out, sizeof out, err, sizeof err),
          "chat_ex succeeds");
    CHECK(strstr(g_body, "\"max_tokens\":400,") != NULL, "chat_ex sends max_tokens");
    CHECK(strstr(g_body, "\"temperature\"") == NULL, "chat_ex omits zero temperature");
    CHECK(np_openai_chat_ex("https://api.example.com", "sk-1", "m", "hi",
                            0.7f, 0, 0, out, sizeof out, err, sizeof err),
          "chat_ex default timeout works");
    CHECK(strstr(g_body, "\"temperature\":0.70,") != NULL, "chat_ex sends temperature");
    CHECK(strstr(g_body, "\"max_tokens\"") == NULL, "chat_ex omits zero max_tokens");

    /* Error path: non-2xx. */
    g_status = 401;
    g_response = NULL;
    out[0] = 'X';
    CHECK(!np_openai_chat("https://api.example.com", "sk-1", "m", "hi",
                          out, sizeof out, err, sizeof err),
          "chat failure reported");
    CHECK(out[0] == 0 && err[0] != 0, "chat failure clears output");

    /* Models: GET /v1/models, collect ids. */
    g_status = 200;
    g_response = "{\"data\":[{\"id\":\"deepseek-chat\",\"object\":\"model\"},"
                 "{\"id\":\"deepseek-reasoner\",\"object\":\"model\"}]}";
    char csv[2048];
    int n = np_openai_list_models("https://api.example.com", "sk-1",
                                  csv, sizeof csv, err, sizeof err);
    CHECK(n == 2, "list models finds 2");
    CHECK(!strcmp(csv, "deepseek-chat,deepseek-reasoner"), "models csv joined");
    CHECK(!strcmp(g_method, "GET"), "models uses GET");
    CHECK(!strcmp(g_url, "https://api.example.com/v1/models"), "models endpoint");
    CHECK(!strcmp(g_bearer, "sk-1"), "models sends api key");

    /* A top-level "id" (request id) must not be offered as a model. */
    g_status = 200;
    g_response = "{\"id\":\"req-abc123\",\"object\":\"list\","
                 "\"data\":[{\"id\":\"gpt-4o\",\"object\":\"model\"}]}";
    n = np_openai_list_models("https://api.example.com", "sk-1",
                              csv, sizeof csv, err, sizeof err);
    CHECK(n == 1 && !strcmp(csv, "gpt-4o"),
          "top-level request id is not a model");

    /* Empty list is an error. */
    g_status = 200;
    g_response = "{\"data\":[]}";
    n = np_openai_list_models("https://api.example.com", "sk-1",
                              csv, sizeof csv, err, sizeof err);
    CHECK(n == -1 && err[0] != 0, "empty model list reported");

    /* ---- Streaming (SSE) ------------------------------------------- */
    delta_calls = 0;
    g_stub_cancel = false;
    g_status = 200;
    g_sse = "data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n"
            "\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n"
            "data: [DONE]\n";
    char sout[256] = {0}, serr[256] = {0};
    volatile bool cancel = false;
    CHECK(np_openai_chat_stream("https://api.example.com", "sk-1", "m", "hi",
                                sout, sizeof sout, serr, sizeof serr,
                                &cancel, delta_probe, NULL),
          "stream returns received text");
    CHECK(!strcmp(sout, "你好"), "stream concatenates deltas");
    CHECK(strstr(g_method, "STREAM") != NULL, "stream uses stream transport");
    CHECK(strstr(g_url, "/v1/chat/completions") != NULL, "stream default endpoint");
    CHECK(strstr(g_body, "\"stream\":true") != NULL, "stream body sets stream true");
    CHECK(delta_calls == 2, "on_delta invoked once per content delta");

    /* Explicit endpoint path (maclaw /api/llm/v1/...). */
    delta_calls = 0;
    g_sse = "data: {\"choices\":[{\"delta\":{\"content\":\"OK\"}}]}\n"
            "data: [DONE]\n";
    sout[0] = 0; serr[0] = 0;
    CHECK(np_openai_chat_stream_url("https://hub.example.com",
                                    "/api/llm/v1/chat/completions", "tok", "auto",
                                    "hi", sout, sizeof sout, serr, sizeof serr,
                                    &cancel, delta_probe, NULL),
          "stream with explicit endpoint succeeds");
    CHECK(!strcmp(sout, "OK"), "stream explicit endpoint text");
    CHECK(!strcmp(g_url, "https://hub.example.com/api/llm/v1/chat/completions"),
          "stream uses explicit endpoint verbatim");
    CHECK(strstr(g_body, "\"model\":\"auto\"") != NULL, "stream carries model auto");

    /* Reject an absolute-URL endpoint (open-redirect guard). */
    sout[0] = 0;
    CHECK(!np_openai_chat_stream_url("https://hub.example.com",
                                     "https://evil.example/x", "tok", "auto",
                                     "hi", sout, sizeof sout, serr, sizeof serr,
                                     &cancel, NULL, NULL),
          "stream rejects absolute endpoint");
    CHECK(strstr(serr, "端点") != NULL, "stream endpoint error reported");

    /* Cancellation: on_delta returning false aborts immediately, partial text
     * is still valid and returned. */
    delta_calls = 0;
    g_sse = "data: {\"choices\":[{\"delta\":{\"content\":\"A\"}}]}\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"B\"}}]}\n"
            "data: [DONE]\n";
    g_stub_cancel = true;
    volatile bool cancel2 = false;
    sout[0] = 0; serr[0] = 0;
    bool cancelled = np_openai_chat_stream("https://api.example.com", "sk-1", "m",
                                           "hi", sout, sizeof sout, serr, sizeof serr,
                                           &cancel2, delta_probe_cancel, NULL);
    CHECK(!cancelled, "cancelled stream reported as failure");
    CHECK(!strcmp(sout, "A"), "partial text preserved on cancel");
    g_stub_cancel = false;
    g_sse = NULL;

    printf("%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
