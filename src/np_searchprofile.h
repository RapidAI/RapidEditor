#ifndef NP_SEARCHPROFILE_H
#define NP_SEARCHPROFILE_H

/* np_searchprofile.h — X9: 可配置知识搜索接口（SearchProfile）.
 *
 * 蓝图红线（X9 / 搜索配置与 API Key 泄漏）：
 *  - 端点、方法、鉴权引用、查询参数名、结果路径、字段映射、条数/超时上限、
 *    域名黑白名单、fallback 全部配置化；默认本地 FTS5 / HTTP JSON 两种后端。
 *  - 密钥只存 auth_ref（钥匙串/环境引用名），明文 API Key 绝不写入配置、
 *    日志或聊天；校验器主动识别常见密钥形态并拒绝（NP_SP_ERR_SECRET_LEAK）。
 *  - 域名白名单为空 = 全部拒绝（fail-closed）；block 优先于 allow；支持
 *    "*.example.com" 通配。未获准的域名不发任何请求。
 *  - 召回结果标题/摘要强制 PII 脱敏（邮箱 / 中国大陆手机号 / 18 位身份证
 *    / 长数字串），响应与单条摘要均有硬上限。
 *  - 只召回：本模块从不写文件、从不执行网页正文；snippet 仅作为不可信数据
 *    返回，调用方不得把它当系统指令。
 *  - fallback 链按 priority 尝试，visited 集合防环；失败降权由调用方持久化。
 *
 * 传输与密钥解析均为注入回调——本模块不链接网络库、不持有任何明文密钥，
 * 因此可在纯本地单测中完整覆盖。真实传输由主程序绑定 np_net_get_json。 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

#define NP_SP_MAX_PROFILES   16
#define NP_SP_NAME_CAP       64
#define NP_SP_URL_CAP        512
#define NP_SP_AUTH_CAP       128   /* 仅引用名，如 keychain:rapideditor/search */
#define NP_SP_FIELD_CAP      64
#define NP_SP_PATH_CAP       256
#define NP_SP_LIST_CAP       512   /* domain_allow / domain_block */
#define NP_SP_TITLE_CAP      160
#define NP_SP_SNIPPET_CAP    512
#define NP_SP_MAX_RESULTS    50
#define NP_SP_RESP_CAP       (512u * 1024u)
#define NP_SP_JSON_MAX_DEPTH 24
#define NP_SP_JSON_MAX_TOKENS 4096

typedef enum {
    NP_SP_HTTP_JSON = 0,     /* 自定义 HTTP JSON 端点（传输注入） */
    NP_SP_LOCAL_FTS = 1      /* 本地 FTS/索引后端（搜索回调注入） */
} np_sp_backend_t;

typedef enum {
    NP_SP_OK = 0,
    NP_SP_ERR_ARG,
    NP_SP_ERR_BAD_URL,
    NP_SP_ERR_SECRET_LEAK,
    NP_SP_ERR_DOMAIN_DENIED,
    NP_SP_ERR_PARSE,
    NP_SP_ERR_HTTP,
    NP_SP_ERR_LIMIT,
    NP_SP_ERR_NO_PROFILE,
    NP_SP_ERR_DISABLED,
    NP_SP_ERR_CYCLE
} np_sp_status_t;

const char *np_sp_status_name(np_sp_status_t s);
const char *np_sp_backend_name(np_sp_backend_t b);

/* 单个搜索配置。所有字符串为定长数组，便于序列化往返与深拷贝。 */
typedef struct {
    char name[NP_SP_NAME_CAP];
    int  backend;                       /* np_sp_backend_t */
    bool enabled;
    char endpoint[NP_SP_URL_CAP];       /* HTTP only */
    char method[8];                     /* "GET" / "POST" */
    char auth_ref[NP_SP_AUTH_CAP];      /* 引用名；禁止明文密钥 */
    char query_param[NP_SP_FIELD_CAP];  /* GET 查询参数名，空 = "q" */
    char result_path[NP_SP_PATH_CAP];   /* 点分路径，如 "data.items"；空 = 根数组 */
    char field_title[NP_SP_FIELD_CAP];
    char field_url[NP_SP_FIELD_CAP];
    char field_snippet[NP_SP_FIELD_CAP];
    int  max_results;                   /* 1..NP_SP_MAX_RESULTS */
    int  timeout_sec;                   /* 1..120 */
    char domain_allow[NP_SP_LIST_CAP];  /* 逗号分隔；空 = 拒绝全部 */
    char domain_block[NP_SP_LIST_CAP];
    char fallback[NP_SP_NAME_CAP];      /* 失败降级目标 profile 名；空 = 无 */
    int  priority;                      /* 小者优先 */
} np_sp_profile_t;

/* 单条召回结果（已脱敏、已截断；不可信数据）。 */
typedef struct {
    char title[NP_SP_TITLE_CAP];
    char url[NP_SP_URL_CAP];
    char snippet[NP_SP_SNIPPET_CAP];
    char source[NP_SP_NAME_CAP];        /* 产出该结果的 profile 名 */
} np_sp_hit_t;

/* ---- 注册表 ---- */
typedef struct np_sp_store np_sp_store_t;
np_sp_store_t *np_sp_store_create(void);
void np_sp_store_destroy(np_sp_store_t *st);
size_t np_sp_count(const np_sp_store_t *st);
const np_sp_profile_t *np_sp_at(const np_sp_store_t *st, size_t i);
const np_sp_profile_t *np_sp_get(const np_sp_store_t *st, const char *name);
int np_sp_index_of(const np_sp_store_t *st, const char *name);
/* 校验通过后拷贝加入（同名覆盖）。 */
np_sp_status_t np_sp_add(np_sp_store_t *st, const np_sp_profile_t *p);
bool np_sp_remove(np_sp_store_t *st, const char *name);
bool np_sp_set_enabled(np_sp_store_t *st, const char *name, bool enabled);

/* ---- 校验 / 序列化 ---- */
np_sp_status_t np_sp_validate(const np_sp_profile_t *p, char *why, size_t why_cap);
/* 明文密钥形态检测（供 UI 在输入时即时提示）。找到返回 true 并写 reason。 */
bool np_sp_looks_like_secret(const char *s, const char **reason);
/* INI 往返：密钥字段只写 auth_ref；任何明文密钥形态会导致保存失败。 */
bool np_sp_store_save(const np_sp_store_t *st, const char *path);
size_t np_sp_store_load(np_sp_store_t *st, const char *path);

/* ---- 域名门禁 / PII ---- */
bool np_sp_domain_allowed(const np_sp_profile_t *p, const char *url);
void np_sp_redact(const char *in, char *out, size_t cap);

/* ---- JSON 路径提取（最小解析器，有界防御）----
 * path 形如 "data.items" / "data.items.0.title"（.序号 = 数组下标）。
 * np_sp_json_slice 返回该节点在原文中的 [start,len)；np_sp_json_string
 * 从一个对象/字符串节点中取出字符串值（处理转义）。 */
bool np_sp_json_slice(const char *json, size_t json_len, const char *path,
                      size_t *out_start, size_t *out_len);
bool np_sp_json_string(const char *json, size_t start, size_t len,
                       const char *key, char *out, size_t cap);

/* ---- 召回（注入传输 / 密钥解析 / 本地搜索）----
 * fetch: 返回堆 buffer（调用方释放）；status_out 可为 NULL。
 *        secret 为已解析的明文密钥——由 resolve 回调持有，不进 profile。
 * resolve: auth_ref -> 明文；返回 NULL 表示无密钥。可为 NULL。 */
typedef char *(*np_sp_fetch_fn)(const char *url, const char *method,
                                const char *secret, int timeout_sec,
                                void *user, int *status_out);
typedef const char *(*np_sp_secret_fn)(const char *auth_ref, void *user);
/* 本地后端：把 query 匹配到的路径写入 out_paths（每个 <= 260 字节）。 */
typedef size_t (*np_sp_local_fn)(const char *query, char out_paths[][260],
                                 size_t cap, void *user);

np_sp_status_t np_sp_recall_http(const np_sp_profile_t *p, const char *query,
                                 np_sp_fetch_fn fetch, np_sp_secret_fn resolve,
                                 void *user, np_sp_hit_t *out, int out_cap,
                                 int *out_total);
np_sp_status_t np_sp_recall_local(const np_sp_profile_t *p, const char *query,
                                  np_sp_local_fn local, void *user,
                                  np_sp_hit_t *out, int out_cap, int *out_total);
/* fallback 链：从 start_name（空=首个启用项）开始，按 fallback/优先级尝试，
 * visited 防环；used_name 接收实际产出结果的 profile。 */
np_sp_status_t np_sp_recall_chain(np_sp_store_t *st, const char *start_name,
                                  const char *query,
                                  np_sp_fetch_fn fetch, np_sp_secret_fn resolve,
                                  np_sp_local_fn local, void *user,
                                  np_sp_hit_t *out, int out_cap, int *out_total,
                                  char *used_name, size_t used_cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_SEARCHPROFILE_H */
