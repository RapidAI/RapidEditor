#ifndef NP_OPENAI_H
#define NP_OPENAI_H

/* Minimal OpenAI-compatible client for user-configured LLM providers
 * (OpenAI / DeepSeek / Qwen / Moonshot / self-hosted vLLM ...). Anything
 * that serves /v1/chat/completions and /v1/models works.
 *
 * All calls are synchronous and intended to run on a worker thread. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_LLM_PROV_NAME_CAP 64
#define NP_LLM_PROV_URL_CAP 256
#define NP_LLM_PROV_KEY_CAP 256
#define NP_LLM_PROV_MODEL_CAP 128
#define NP_LLM_PROV_MODELS_CAP 4096

typedef struct {
    char name[NP_LLM_PROV_NAME_CAP];    /* display name, e.g. "DeepSeek" */
    char url[NP_LLM_PROV_URL_CAP];      /* base URL, e.g. https://api.deepseek.com */
    char api_key[NP_LLM_PROV_KEY_CAP];
    char model[NP_LLM_PROV_MODEL_CAP];  /* selected model id */
    char models[NP_LLM_PROV_MODELS_CAP];/* cached model ids, comma-separated */
    float temperature;                  /* 0 = provider default (omitted) */
    int   max_tokens;                   /* 0 = provider default (omitted) */
} np_llm_provider;

/* Optional sampling parameters applied to the next chat / stream calls made
 * by THIS thread's workers. Zero values omit the JSON fields entirely. */
void np_openai_set_params(float temperature, int max_tokens);

/* One non-streaming chat completion. Returns true and writes
 * choices[0].message.content to `out`; errors go to `err`. */
bool np_openai_chat(const char *base_url, const char *api_key,
                    const char *model, const char *prompt,
                    char *out, size_t out_cap,
                    char *err, size_t err_cap);

/* Variant with per-call sampling params and timeout: temperature/max_tokens
 * follow the np_llm_provider convention (0 = omit the JSON field), while
 * timeout_sec overrides the default 120s. Lets lightweight callers (inline
 * completion) stay cheap and fail fast without touching the global params
 * set via np_openai_set_params. */
bool np_openai_chat_ex(const char *base_url, const char *api_key,
                       const char *model, const char *prompt,
                       float temperature, int max_tokens, int timeout_sec,
                       char *out, size_t out_cap,
                       char *err, size_t err_cap);

/* Streaming chat completion for OpenAI-compatible SSE endpoints. `out` is
 * appended to as each `content` delta arrives; `on_delta` is invoked with the
 * decoded delta (may be NULL). Returning false from `on_delta` aborts. */
typedef bool (*np_llm_delta_cb)(const char *delta, size_t len, void *user);
bool np_openai_chat_stream(const char *base_url, const char *api_key,
                           const char *model, const char *prompt,
                           char *out, size_t out_cap,
                           char *err, size_t err_cap,
                           volatile bool *cancel,
                           np_llm_delta_cb on_delta, void *user);

/* GET /v1/models and collect the model ids into `out_csv` (comma-separated,
 * bounded by `cap`). Returns the number of models found, or -1 on error. */
int np_openai_list_models(const char *base_url, const char *api_key,
                          char *out_csv, size_t cap,
                          char *err, size_t err_cap);

/* Extract a string field from a JSON object (handles escapes). */
bool np_openai_json_get_string(const char *json, const char *key,
                               char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_OPENAI_H */
