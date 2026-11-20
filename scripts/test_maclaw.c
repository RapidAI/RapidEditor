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
