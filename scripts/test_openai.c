/* Unit tests for the OpenAI-compatible provider module (stub transport). */
#include "np_openai.h"
#include "np_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static char g_url[1200], g_bearer[512], g_body[65536], g_method[16];
static const char *g_response;
static int g_status;

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

/* Streaming transport stub: these tests never exercise the stream path. */
bool np_net_post_json_stream(const char *url, const char *bearer, const char *body,
                             const np_net_opts *o, int *status_out,
                             bool (*chunk_cb)(const char *, size_t, void *), void *user) {
    (void)url; (void)bearer; (void)body; (void)o; (void)chunk_cb; (void)user;
    if (status_out) *status_out = 0;
    return false;
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

    printf("%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
