/* np_revasst.h — R5: AI 逆向助手（只读分析，命名待确认）.
 *
 * 文档要求（R5 · S2 · P0）：看懂指令后仍难形成语义结论。
 * 选函数/指令 → 解释、命名建议、调用链摘要、可疑行为标记；
 * 命名一律带「待确认」；不写入原文。
 *
 * 安全红线：本模块<b>只读</b>——消费 R2 已解码指令与 R3 CFG，
 * 只产出分析文本与建议，绝不修改任何源字节或文件。所有命名候选强制
 * 标记「待确认」，AI 输出视作不可信输入（清洗控制字符 + 有界截断），
 * 每条结论携带 file:0xOFFSET 或 file#func 引用供回跳核验。
 *
 * 诚实边界：本地启发式（可疑行为/命名/调用链）基于指令文本与 CFG 图的
 * 模式匹配，不是语义理解；AI 解释通过注入回调委托外部后端，本模块不
 * 硬链接任何 LLM/网络栈。调用链仅静态 CALL 边，不含间接调用/动态解析。
 */
#ifndef NP_REVASST_H
#define NP_REVASST_H

#include "np_cfg.h"
#include "np_disasm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_RA_MAX_CTX     24576      /* 分析上下文包上限 */
#define NP_RA_MAX_OUT     24576      /* 报告输出上限 */
#define NP_RA_MAX_TEXT    400        /* 单条解释文本上限 */
#define NP_RA_MAX_NAME    96
#define NP_RA_MAX_SUSPECT 32
#define NP_RA_MAX_CHAIN   64
#define NP_RA_MAX_REF     200

typedef enum {
    NP_RA_OK = 0,
    NP_RA_ERR_ARG,
    NP_RA_ERR_FULL,
    NP_RA_ERR_NOCFG,
    NP_RA_ERR_UNSUPPORTED
} np_ra_status_t;
const char *np_ra_status_name(np_ra_status_t s);

/* 可疑行为类别（只读标记，非判定恶意） */
typedef enum {
    NP_RA_S_SYSCALL = 0,     /* syscall / int 0x80 / svc */
    NP_RA_S_NETWORK,         /* 网络相关导入/字符串（socket/send/connect） */
    NP_RA_S_FILE,            /* 文件操作（open/read/write/fopen） */
    NP_RA_S_CRYPTO,          /* 加密特征（aes/sha/md5/xor 循环） */
    NP_RA_S_ANTIDEBUG,       /* 反调试（ptrace/isdebuggerpresent/rdtsc） */
    NP_RA_S_PRIVILEGE,       /* 提权/权限（setuid/adjusttoken） */
    NP_RA_S_PROCESS,         /* 进程/注入（createthread/virtualalloc/dlopen） */
    NP_RA_S_OTHER
} np_ra_suspect_kind_t;
const char *np_ra_suspect_name(np_ra_suspect_kind_t k);

/* 单条可疑标记 */
typedef struct {
    np_ra_suspect_kind_t kind;
    size_t insn_index;       /* 触发指令下标 */
    uint64_t addr;
    char   evidence[NP_RA_MAX_TEXT];  /* 触发文本（有界） */
    char   ref[NP_RA_MAX_REF];        /* file:0xOFFSET 引用 */
    int    conf;             /* 0..100 启发式置信度 */
} np_ra_suspect_t;

/* 命名候选（强制待确认） */
typedef struct {
    char   name[NP_RA_MAX_NAME];      /* 建议名（如 "parse_header"） */
    char   basis[NP_RA_MAX_TEXT];     /* 命名依据 */
    int    conf;
    bool   needs_confirm;             /* 恒为 true（红线） */
} np_ra_name_t;

/* 调用链条目 */
typedef struct {
    size_t   func_index;
    uint64_t addr;
    char     name[NP_RA_MAX_NAME];
    char     ref[NP_RA_MAX_REF];
} np_ra_chain_item_t;

/* 逆向分析结果（针对一个函数） */
typedef struct {
    int     func_index;
    uint64_t func_addr;
    char    func_name[NP_RA_MAX_NAME];
    char    explanation[NP_RA_MAX_TEXT];   /* 解释（本地启发式或 AI） */
    bool    explanation_from_ai;           /* 是否来自注入的 AI 回调 */
    np_ra_name_t name;
    char    chain_summary[NP_RA_MAX_TEXT]; /* 调用链摘要 */
    np_ra_chain_item_t chain[NP_RA_MAX_CHAIN];
    int     nchain;
    np_ra_suspect_t suspects[NP_RA_MAX_SUSPECT];
    int     nsuspects;
    char    source[NP_RA_MAX_REF];         /* 引用协议文件名 */
} np_ra_result_t;

/* AI 解释回调：输入是已构建的分析上下文包（不可信输入由本模块清洗），
 * 回调把解释写入 out（有界 cap）。返回 true 表示成功；false 时本模块
 * 回退到本地启发式解释。user 为回调自有状态。 */
typedef bool (*np_ra_explain_fn)(const char *context, char *out, size_t cap,
                                 void *user);

/* 构建分析上下文包：把选中函数的指令 + CFG 边 + 地址引用整理成
 * 结构化文本，供 AI 回调或人工审阅。纯只读。insns/n 为 R2 已解码指令
 * （与构建 g 时相同）；传 NULL 时上下文仅含 CFG 摘要。 */
np_ra_status_t np_ra_build_context(const char *source,
                                   const np_cfg_graph_t *g, int func_index,
                                   const np_disasm_insn_t *insns, size_t n,
                                   char *out, size_t cap);

/* 分析一个函数：本地启发式（可疑行为/命名候选/调用链）+ 可选 AI 解释。
 * explain 为 NULL 时仅用本地启发式；explain 返回 false 时回退本地。
 * insns/n 为 R2 已解码指令，用于可疑行为模式匹配与上下文包（可传 NULL，
 * 此时仅基于 CFG 摘要）。不改任何指令/字节。 */
np_ra_status_t np_ra_analyze(const char *source, const np_cfg_graph_t *g,
                             int func_index,
                             const np_disasm_insn_t *insns, size_t n,
                             np_ra_explain_fn explain, void *user,
                             np_ra_result_t *out);

/* 渲染为 Markdown 报告（解释 + 命名待确认 + 调用链 + 可疑行为 + provenance）。 */
int np_ra_render(const np_ra_result_t *r, char *out, size_t cap);

/* 不可信文本清洗（AI 输出 / 导入字符串）：剥离控制字符、有界截断。 */
void np_ra_sanitize(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_REVASST_H */
