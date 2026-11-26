/* np_mcp.h — S1: X6 MCP 工具服务器（动态发现 / 健康检查 / 隔离运行）.
 *
 * 文档要求（S1 · X 系列 · X6）：
 *  - 本地 stdio 与远程 HTTP 两种 profile；启动时发现工具、周期健康检查；
 *  - 工具输出视为不可信输入（脱敏 + 控制字符剥离 + 有界截断），写入统一回到
 *    Patch Job（本模块默认禁写，写工具须显式 allow_write + 上层审查）；
 *  - 密钥只存系统钥匙串引用（keychain:/env:/vault:），不进聊天/日志/配置明文；
 *  - 未通过健康检查不注册；连续失败自动禁用；域名白名单 fail-closed（block 优先）。
 *
 * 纯本地、UI-free、零网络依赖：传输（stdio/HTTP 进程外调用）与密钥解析全注入。
 * 与 np_searchprofile 同构，但独立编译（run_tests 各套件只链自身依赖）。
 */
#ifndef NP_MCP_H
#define NP_MCP_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_MCP_MAX_SERVERS 32
#define NP_MCP_MAX_TOOLS   64
#define NP_MCP_RESP_CAP    (512 * 1024)   /* 单次响应上限 512KB */
#define NP_MCP_OUT_CAP     8192           /* 工具输出进入上下文前的硬上限 */
#define NP_MCP_FAIL_LIMIT  3              /* 连续失败 3 次自动禁用 */

typedef enum { NP_MCP_STDIO = 0, NP_MCP_HTTP = 1 } np_mcp_transport_t;

typedef enum {
    NP_MCP_OK = 0,
    NP_MCP_ERR_ARG,
    NP_MCP_ERR_BAD_CONFIG,
    NP_MCP_ERR_SECRET_LEAK,
    NP_MCP_ERR_DOMAIN_DENIED,
    NP_MCP_ERR_TRANSPORT,
    NP_MCP_ERR_PARSE,
    NP_MCP_ERR_HEALTH,
    NP_MCP_ERR_DISABLED,
    NP_MCP_ERR_NOT_FOUND,
    NP_MCP_ERR_LIMIT,
    NP_MCP_ERR_WRITE_DENIED          /* 写工具在未显式允许时被拒绝 */
} np_mcp_status_t;

const char *np_mcp_status_name(np_mcp_status_t s);
const char *np_mcp_transport_name(np_mcp_transport_t t);

/* ---------------- Server profile ----------------
 * 只保存"引用"与"声明"，不保存任何明文凭据/密钥。 */
typedef struct {
    char                name[64];
    np_mcp_transport_t  transport;
    char                command[256];     /* stdio: 启动命令（禁 shell 元字符） */
    char                endpoint[256];    /* http: 完整 URL */
    char                auth_ref[128];    /* keychain:/env:/vault:... 引用 */
    char                domain_allow[256];/* http: 空格/逗号分隔，空= fail-closed */
    char                domain_block[256];/* http: 黑名单，优先于 allow */
    int                 timeout_sec;      /* 1..120 */
    bool                enabled;
    bool                health;           /* 最近一次健康检查通过与否 */
    int                 fail_streak;      /* 连续失败次数，达 3 自动禁用 */
    bool                allow_write;      /* 默认 false：写工具禁调用 */
    char                sha256[65];       /* 包/配置完整性（可选） */
} np_mcp_server_t;

/* 动态发现的工具（来自 tools/list，按不可信输入处理）。 */
typedef struct {
    char    name[80];
    char    description[240];
    bool    write_tool;     /* 名称/描述含写动词 → 需 allow_write 才能调用 */
    bool    enabled;
} np_mcp_tool_t;

/* JSON-RPC 请求-响应传输回调。
 * request 为完整 JSON-RPC 请求串；实现方执行 stdio 子进程或 HTTP 调用，
 * 把原始响应写入 out_resp（含 NUL），成功返回 true。不得把密钥拼进 request。 */
typedef bool (*np_mcp_transport_fn)(const np_mcp_server_t *srv,
                                    const char *request, char *out_resp,
                                    size_t out_cap, void *user);
/* 把 auth_ref 解析为运行时凭据（来自系统钥匙串）；仅内部传给传输实现，
 * 不进入日志/上下文。返回 NULL 表示无法解析。 */
typedef const char *(*np_mcp_secret_fn)(const char *auth_ref, void *user);

/* ---------------- Store ---------------- */
typedef struct np_mcp_store np_mcp_store_t;

np_mcp_store_t *np_mcp_store_create(void);
void np_mcp_store_destroy(np_mcp_store_t *st);

/* 配置校验：非法返回状态并写 why。密钥必须是引用形态；明文形态一律拒绝。 */
np_mcp_status_t np_mcp_validate(const np_mcp_server_t *s, char *why, size_t why_cap);
/* 只识别常见明文密钥前缀/内联赋值/高熵串；keychain:/env:/vault: 引用不命中。 */
bool np_mcp_looks_like_secret(const char *s, const char **reason);
/* http 域名门禁：block 优先；allow 空则拒绝（fail-closed）；支持 *. 通配后缀。 */
bool np_mcp_domain_allowed(const np_mcp_server_t *s, const char *url);

/* 增删启停（增时校验 + 去重）。 */
np_mcp_status_t np_mcp_add(np_mcp_store_t *st, const np_mcp_server_t *s);
bool np_mcp_remove(np_mcp_store_t *st, const char *name);
bool np_mcp_set_enabled(np_mcp_store_t *st, const char *name, bool enabled);
/* 显式允许/禁止写工具（默认 false；开启需上层确认权限）。 */
bool np_mcp_set_allow_write(np_mcp_store_t *st, const char *name, bool allow);

size_t np_mcp_count(const np_mcp_store_t *st);
int np_mcp_index_of(const np_mcp_store_t *st, const char *name);
const np_mcp_server_t *np_mcp_at(const np_mcp_store_t *st, size_t i);
const np_mcp_server_t *np_mcp_get(const np_mcp_store_t *st, const char *name);

/* 持久化（mcp_servers.ini）：只写引用，不写明文。 */
bool np_mcp_store_save(const np_mcp_store_t *st, const char *path);
size_t np_mcp_store_load(np_mcp_store_t *st, const char *path);

/* ---------------- 动态发现 / 健康检查 / 调用 ----------------
 * 三者都经由注入的 transport/secret；失败不抛异常，返回状态。 */

/* 启动时发现：发 tools/list 请求，解析 result.tools[]，写入 store 的该 server；
 * name/description 经不可信输出清洗（PII 脱敏 + 控制字符剥离 + 有界截断）。
 * 返回发现到的工具数（0 表示失败，具体原因见可选 out_status）。 */
int np_mcp_discover(np_mcp_store_t *st, const char *name,
                    np_mcp_transport_fn transport, np_mcp_secret_fn resolve,
                    void *user, np_mcp_status_t *out_status);
/* 周期健康检查：发 ping 请求；成功 health=true/fail_streak=0，失败累计，
 * 达 NP_MCP_FAIL_LIMIT 自动 enabled=false（失败禁用）。返回是否健康。 */
bool np_mcp_health_check(np_mcp_store_t *st, const char *name,
                         np_mcp_transport_fn transport, np_mcp_secret_fn resolve,
                         void *user);
/* 调用只读工具：write_tool 且 !allow_write → NP_MCP_ERR_WRITE_DENIED；
 * 未启用/未通过健康检查 → NP_MCP_ERR_DISABLED；http 域名不合格 → DOMAIN_DENIED。
 * 返回结果中的不可信文本（脱敏 + 截断）写入 out。 */
np_mcp_status_t np_mcp_call(np_mcp_store_t *st, const char *name,
                            const char *tool, const char *arguments_json,
                            np_mcp_transport_fn transport, np_mcp_secret_fn resolve,
                            void *user, char *out, size_t out_cap);

/* 该 server 当前发现到的工具（只读视图；索引 0..np_mcp_tool_count-1）。 */
size_t np_mcp_tool_count(const np_mcp_store_t *st, const char *name);
const np_mcp_tool_t *np_mcp_tool_at(const np_mcp_store_t *st, const char *name, size_t i);
bool np_mcp_set_tool_enabled(np_mcp_store_t *st, const char *name,
                             const char *tool, bool enabled);

/* 不可信输出清洗：PII 脱敏 + 控制字符剥离 + 有界截断（长度保留标注）。
 * UI 展示工具 name/description/结果文本前必须经过此函数。 */
void np_mcp_sanitize(const char *in, char *out, size_t cap);

/* ---------------- 最小 JSON-RPC 辅助（供传输实现复用，UI-free） ----------------
 * 只定位/取串，不构建树；深度与 token 双重上限。 */

/* 构造 JSON 字符串字面量（含转义），写入 out。 */
void np_mcp_json_escape(const char *s, char *out, size_t cap);
/* 从 JSON 响应里按点分路径取字符串（如 "result.content.0.text"）。 */
bool np_mcp_json_string(const char *json, size_t json_len, const char *path,
                        char *out, size_t cap);
/* 取数组元素个数（路径指向数组），用于 tools/list 解析。 */
int np_mcp_json_array_len(const char *json, size_t json_len, const char *path);
/* 取数组第 idx 个元素节点的 [start,len)，供进一步取字段。 */
bool np_mcp_json_array_elem(const char *json, size_t json_len, const char *path,
                            int idx, size_t *out_start, size_t *out_len);
/* 从对象节点里取 key 的字符串值。 */
bool np_mcp_json_member_string(const char *json, size_t start, size_t len,
                               const char *key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_MCP_H */
