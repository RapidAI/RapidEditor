/* np_extlife.h — X12: 扩展生命周期审计、回滚与清理.
 *
 * 文档要求（X12 · S2 · P1）：自动安装与卸载若不可追溯，会留下凭据、缓存和
 * 悬挂工具定义。安装/更新/删除都是 Job；记录来源、哈希、许可证、权限与
 * 调用摘要；删除事务清理凭据/缓存/工具注册，仅保留审计墓碑；失败一键回滚。
 *
 * 安全红线：
 *  - 账本<b>只记元数据</b>（来源/哈希/许可证/权限/凭据<b>引用</b>），
 *    绝不保存明文密钥/token；
 *  - 删除是<b>事务</b>：注入式清理回调依次清理凭据引用/缓存/工具注册，
 *    任一失败即回滚（恢复注册 + 条目重新激活），不留悬挂定义；
 *  - 删除成功后只留<b>审计墓碑</b>（tombstone=true，条目内容保留可追溯），
 *    清理产物不落明文；
 *  - 全部只读审计数据 + 显式注入的清理动作，本模块不直接删磁盘文件、
 *    不访问网络、不硬链接任何后端。
 *
 * 诚实边界：回滚只恢复本模块账本状态 + 调用方注入的 restore 回调；
 * 真正的凭据/缓存/注册恢复由注入回调完成，未注入时明确标记未回滚。
 */
#ifndef NP_EXTLIFE_H
#define NP_EXTLIFE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_XL_MAX_ENTRIES   128
#define NP_XL_MAX_ID        96
#define NP_XL_MAX_TEXT      200
#define NP_XL_MAX_REF       200
#define NP_XL_MAX_PERMS     8
#define NP_XL_MAX_OUT       16384
#define NP_XL_MAX_SNAPSHOT  16

typedef enum {
    NP_XL_OK = 0,
    NP_XL_ERR_ARG,
    NP_XL_ERR_NOT_FOUND,
    NP_XL_ERR_FULL,
    NP_XL_ERR_DUP,
    NP_XL_ERR_TXN_FAILED,     /* 清理事务失败（已回滚） */
    NP_XL_ERR_IO
} np_xl_status_t;
const char *np_xl_status_name(np_xl_status_t s);

typedef enum {
    NP_XL_INSTALL = 0,
    NP_XL_UPDATE,
    NP_XL_REMOVE
} np_xl_op_t;
const char *np_xl_op_name(np_xl_op_t op);

typedef enum {
    NP_XL_ACTIVE = 0,
    NP_XL_CLEANING,
    NP_XL_REMOVED,
    NP_XL_ROLLED_BACK
} np_xl_state_t;
const char *np_xl_state_name(np_xl_state_t s);

/* 审计条目（只含元数据，无明文凭据） */
typedef struct {
    np_xl_op_t  op;
    np_xl_state_t state;
    uint64_t    job_id;           /* A1 Job 关联 */
    char        id[NP_XL_MAX_ID];          /* 扩展 id（skill/MCP 名） */
    char        source[NP_XL_MAX_REF];     /* 来源 URL / 路径 */
    char        sha256[65];                /* 内容哈希（SHA256 hex） */
    char        license[NP_XL_MAX_TEXT];
    char        perms[NP_XL_MAX_PERMS][NP_XL_MAX_TEXT]; /* 权限/范围声明 */
    int         nperms;
    char        auth_ref[NP_XL_MAX_REF];   /* 凭据引用（keychain:/env:...），非明文 */
    char        cache_path[NP_XL_MAX_REF]; /* 缓存路径（仅元数据） */
    char        tool_ref[NP_XL_MAX_REF];   /* 工具注册引用 */
    char        call_summary[NP_XL_MAX_TEXT]; /* 调用/用途摘要 */
    char        timestamp[40];    /* 注入的时钟字符串 */
    bool        tombstone;        /* 删除成功后仅留墓碑 */
} np_xl_entry_t;

typedef struct {
    int      n;
    np_xl_entry_t entries[NP_XL_MAX_ENTRIES];
    char     source[NP_XL_MAX_REF];  /* 账本自身来源标记 */
} np_xl_ledger_t;

/* 事务清理回调：按 kind 清理一类资源。ctx 由调用方提供；
 * 返回 true 成功，false 失败（触发整笔回滚）。本模块不直接删文件。 */
typedef enum {
    NP_XL_CLEAR_CREDENTIAL = 0,   /* 清理 auth_ref 指向的凭据 */
    NP_XL_CLEAR_CACHE,            /* 清理 cache_path */
    NP_XL_CLEAR_TOOLREG           /* 清理 tool_ref 工具注册 */
} np_xl_clear_kind_t;

typedef bool (*np_xl_clear_fn)(np_xl_clear_kind_t kind, const char *value,
                               void *user);
/* 回滚回调：kind 对应资源需要恢复（传入事务前保存的快照值）。 */
typedef bool (*np_xl_restore_fn)(np_xl_clear_kind_t kind, const char *value,
                                 void *user);
/* 时钟注入：返回可读时间戳字符串（写入 buf）。NULL 用 "unknown"。 */
typedef void (*np_xl_clock_fn)(char *buf, size_t cap, void *user);

/* 初始化/清空账本 */
void np_xl_init(np_xl_ledger_t *l, const char *source);

/* 记录一次安装/更新/删除 Job（只记元数据；重复 id+op+job_id 拒绝）。
 * clock 为 NULL 时 timestamp="unknown"。 */
np_xl_status_t np_xl_record(np_xl_ledger_t *l, np_xl_op_t op, uint64_t job_id,
                            const np_xl_entry_t *detail,
                            np_xl_clock_fn clock, void *user);

/* 按 id 查最新条目（含墓碑）；不存在返回 NULL。 */
const np_xl_entry_t *np_xl_latest(const np_xl_ledger_t *l, const char *id);

/* 事务性卸载：按 id 找 active 条目 → 标记 CLEANING → 依次调用 clear
 * （credential/cache/toolreg，只对非空引用调用）→ 全成功则标记 REMOVED +
 * tombstone=true；任一失败则对所有已清理项调用 restore、标记 ROLLED_BACK。
 * 未注入 clear 时仍可标记 REMOVED（无外部资源可清），但不伪造回滚能力。 */
np_xl_status_t np_xl_uninstall(np_xl_ledger_t *l, const char *id,
                               np_xl_clear_fn clear, np_xl_restore_fn restore,
                               void *user, bool *rolled_back);

/* 回滚：对最近一次 REMOVED/ROLLED_BACK 条目调用 restore 并恢复为 ACTIVE
 * （tombstone 保留可追溯）；未注入 restore 时返回 ERR_ARG 且状态不变。 */
np_xl_status_t np_xl_rollback(np_xl_ledger_t *l, const char *id,
                              np_xl_restore_fn restore, void *user);

/* 账本 INI 往返（只存元数据；perms 用 | 分隔）。 */
np_xl_status_t np_xl_save(const np_xl_ledger_t *l, const char *path);
np_xl_status_t np_xl_load(np_xl_ledger_t *l, const char *path);

/* 渲染 Markdown 审计报告（含 provenance、墓碑/回滚状态、诚实边界）。 */
int np_xl_render(const np_xl_ledger_t *l, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_EXTLIFE_H */
