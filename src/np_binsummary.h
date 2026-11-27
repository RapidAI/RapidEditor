/* np_binsummary.h — R4: 符号 / 字符串 / 导入表摘要.
 *
 * 文档要求（R4 · S1 · P1）：只有字节，没有"这个程序在做什么"的入口。
 * 解析符号表、可打印字符串与导入函数；AI 分类并生成一页报告；
 * 每条结论带 offset 引用。
 *
 * 安全红线：本模块<b>只读</b>，绝不修改源字节；所有表偏移/数量/字符串长度
 * 有硬上限，畸形文件不会产生越界读或耗尽内存。每条结论带 file:0xOFFSET
 * 引用供回跳核验。AI 分类通过注入回调委托外部（与 R5 同构），本模块不硬链
 * 任何 LLM/网络栈。
 *
 * 诚实边界：ELF 只解析节头表存在的 .dynsym/.symtab + 关联字符串表
 * （stripped 无 symtab 时只报 dynsym）；PE 只解析 IMAGE_IMPORT_DESCRIPTOR
 * 按 DLL 分组的函数名；字符串提取是长度阈值 + 可打印判定，不做语义分类。
 * 本模块不加载/执行二进制，不做重定位或解密。
 */
#ifndef NP_BINSUMMARY_H
#define NP_BINSUMMARY_H

#include "np_probe.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_BS_MAX_SYMBOLS  256
#define NP_BS_MAX_IMPORTS  256
#define NP_BS_MAX_STRINGS  96     /* 控制结果结构体积，避免 UI 栈帧过大 */
#define NP_BS_MAX_DLLS     32
#define NP_BS_MAX_NAME     120
#define NP_BS_MAX_REF      160
#define NP_BS_MAX_OUT      16384
#define NP_BS_MIN_STRLEN   4
#define NP_BS_MAX_STRLEN   256

typedef enum {
    NP_BS_OK = 0,
    NP_BS_ERR_ARG,
    NP_BS_ERR_NOTPROG,       /* 非 ELF/PE 或不支持的格式 */
    NP_BS_ERR_PARSE,         /* 头/表结构非法 */
    NP_BS_ERR_FULL
} np_bs_status_t;
const char *np_bs_status_name(np_bs_status_t s);

typedef enum { NP_BS_ELF = 0, NP_BS_PE } np_bs_format_t;
const char *np_bs_format_name(np_bs_format_t f);

/* 符号 */
typedef enum {
    NP_BS_SYM_GLOBAL = 0,
    NP_BS_SYM_WEAK,
    NP_BS_SYM_LOCAL,
    NP_BS_SYM_OTHER
} np_bs_sym_bind_t;

typedef struct {
    np_bs_sym_bind_t bind;
    bool     undefined;       /* ELF st_shndx==SHN_UNDEF = 导入符号 */
    bool     is_func;         /* ELF STT_FUNC / PE 按导入 */
    uint64_t value;           /* st_value / 0 */
    char     name[NP_BS_MAX_NAME];
    char     ref[NP_BS_MAX_REF];
} np_bs_symbol_t;

/* 导入函数（按 DLL/来源分组） */
typedef struct {
    char     lib[NP_BS_MAX_NAME];   /* DLL 名 / "ELF:<soname>" / "(unknown)" */
    char     name[NP_BS_MAX_NAME];
    char     ref[NP_BS_MAX_REF];
    bool     by_ordinal;           /* PE 按序号导入 */
    uint16_t ordinal;
} np_bs_import_t;

/* 可打印字符串 */
typedef struct {
    size_t   offset;          /* 文件中的字节偏移 */
    size_t   len;
    char     text[NP_BS_MAX_STRLEN];
    char     ref[NP_BS_MAX_REF];
    bool     truncated;
} np_bs_string_t;

typedef struct {
    np_bs_format_t format;
    np_parch_t   arch;
    int          bits;
    int          nsymbols;
    np_bs_symbol_t symbols[NP_BS_MAX_SYMBOLS];
    int          nimports;
    np_bs_import_t imports[NP_BS_MAX_IMPORTS];
    int          nstrings;
    np_bs_string_t strings[NP_BS_MAX_STRINGS];
    int          ndlls;            /* 导入来源（DLL）去重数 */
    bool         symbols_truncated;
    bool         imports_truncated;
    bool         strings_truncated;
    char         source[NP_BS_MAX_REF];
    /* AI 分类结果（注入回调填充；未注入为空） */
    char         ai_summary[NP_BS_MAX_OUT];
    bool         ai_used;
} np_bs_result_t;

/* AI 分类回调（注入）：输入是一页文本摘要，输出是分类/用途摘要（有界）。
 * 返回 true 表示成功；失败/缺失时报告仅含确定性解析结论。 */
typedef bool (*np_bs_classify_fn)(const char *digest, char *out, size_t cap,
                                  void *user);

/* 从内存缓冲解析 ELF/PE 摘要（只读）。source 用于引用协议文件名。 */
np_bs_status_t np_bs_parse(const unsigned char *data, size_t len,
                           const char *source, np_bs_result_t *out);

/* 可选 AI 分类：对已解析结果调用注入回调，把结果存入 r->ai_summary。
 * 未调用时报告只含确定性解析结论。只读，不改源字节。 */
np_bs_status_t np_bs_ai_classify(np_bs_result_t *r, np_bs_classify_fn fn,
                                 void *user);

/* 从磁盘文件解析（封装读文件 + np_bs_parse，64MB 上限）。 */
np_bs_status_t np_bs_parse_file(const char *path, np_bs_result_t *out);

/* 渲染一页 Markdown 报告（符号/导入/字符串 + offset 引用 + provenance）。
 * 容量不足返回 -1。 */
int np_bs_render(const np_bs_result_t *r, char *out, size_t cap);

/* 构建供 AI 分类的确定性摘要文本（符号名/导入名/字符串，有界）。 */
int np_bs_digest(const np_bs_result_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_BINSUMMARY_H */
