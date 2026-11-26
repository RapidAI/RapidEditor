/* np_byteintel.h — H3 字节解释 + H4 结构化字节搜索（二进制轨，只读）.
 *
 * 文档要求：
 *  H3（S1·P1）：选区字节 → 偏移/长度/类型/端序说明；输出字段芯片可跳 hex；只读。
 *  H4（S1·P1）：结构化字节搜索——"找小端 u32=1000""找 PNG 块""找可疑填充区"；
 *             本地预筛 + 命中刻度（复用滚动条 tick）。
 *
 * 安全/可信边界：纯本地确定性逻辑，不写任何字节，不调用模型；解释器只给
 * "事实 + 候选"（类型/端序/值），是否采信由用户决定。查询有长度/次数硬上限，
 * 畸形输入只返回错误，不崩溃。
 */
#ifndef NP_BYTEINTEL_H
#define NP_BYTEINTEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_BI_MAX_FIELDS  24
#define NP_BI_MAX_HITS    512      /* 与 hex 滚动条 tick 上限对齐 */
#define NP_BI_VALUE_CAP   96
#define NP_BI_QUERY_CAP   128      /* 查询文本最大长度 */
#define NP_BI_STR_CAP     64

/* ---------------- H3：字节解释 ---------------- */

typedef enum {
    NP_BI_U8 = 0, NP_BI_I8,
    NP_BI_U16LE, NP_BI_U16BE, NP_BI_I16LE, NP_BI_I16BE,
    NP_BI_U32LE, NP_BI_U32BE, NP_BI_I32LE, NP_BI_I32BE,
    NP_BI_U64LE, NP_BI_U64BE,
    NP_BI_F32LE, NP_BI_F64LE,
    NP_BI_ASCII, NP_BI_IPV4, NP_BI_MAC, NP_BI_GUID
} np_bi_type_t;

const char *np_bi_type_name(np_bi_type_t t);
/* 端序标签："LE" / "BE" / ""（无端序概念，如 ASCII）。 */
const char *np_bi_type_endian(np_bi_type_t t);

typedef struct {
    np_bi_type_t type;
    size_t   offset;               /* 字段起始偏移（= 选区起点） */
    size_t   size;                 /* 字段字节数 */
    char     value[NP_BI_VALUE_CAP];
    bool     plausible;            /* 启发式"看起来合理"（如 ASCII 全可打印） */
} np_bi_field_t;

typedef struct {
    int            nfields;
    np_bi_field_t  fields[NP_BI_MAX_FIELDS];
} np_bi_result_t;

/* 从 data[offset, offset+length) 解释字节，产出多类型候选字段。
 * 只读。offset/length 越界会被钳制到缓冲区；length 过大只解释前若干字节。 */
void np_bi_interpret(const unsigned char *data, size_t len,
                     size_t offset, size_t length, np_bi_result_t *out);

/* ---------------- H4：结构化字节搜索 ---------------- */

typedef enum {
    NP_BIQ_UINT_EQ = 0,   /* uNle|uNbe|iNle|iNbe = 值   */
    NP_BIQ_UINT_GT,       /* 比较：> >= < <=             */
    NP_BIQ_UINT_GE,
    NP_BIQ_UINT_LT,
    NP_BIQ_UINT_LE,
    NP_BIQ_SIG,           /* 已知签名：elf/pe/jpg/png/gif/zip/bmp     */
    NP_BIQ_CHUNK,         /* png:CHUNK（PNG 块类型 ASCII）            */
    NP_BIQ_FILL,          /* fill:0xNN:minrun（连续相同字节）         */
    NP_BIQ_STR            /* str:text（字面可打印串）                 */
} np_bi_query_kind_t;

typedef enum { NP_BI_CMP_EQ, NP_BI_CMP_GT, NP_BI_CMP_GE,
               NP_BI_CMP_LT, NP_BI_CMP_LE } np_bi_cmp_t;

typedef struct {
    np_bi_query_kind_t kind;
    int        width;              /* 整数宽度 1/2/4/8 */
    bool       big_endian;
    bool       signed_val;
    np_bi_cmp_t cmp;              /* UINT_* 用 */
    uint64_t   value;             /* UINT_* 比较目标（有符号按补码存） */
    char       sig[8];            /* SIG / CHUNK 关键字 */
    size_t     sig_len;           /* sig 有效字节数（二进制签名可能含 NUL） */
    unsigned char fill_byte;      /* FILL */
    size_t     min_run;           /* FILL 最小连续长度 */
    char       str[NP_BI_STR_CAP];/* STR */
    char       desc[160];         /* 规范化描述（展示用） */
} np_bi_query_t;

typedef struct {
    size_t offset;
    size_t length;
} np_bi_hit_t;

/* 解析查询文本。支持形式（大小写不敏感的关键字）：
 *   u16le=1000 / u32be=0xDEADBEEF / i32le=-5
 *   u16le>10  / u32le>=0x100 / u8<=255 / i64lt=-1
 *   elf / pe / jpg / png / gif / zip / bmp
 *   png:IHDR / png:IDAT
 *   fill:0x00:16 / fill:FF:8
 *   str:hello
 * 失败返回 false 并写 err（可为空）。*/
bool np_bi_query_parse(const char *text, np_bi_query_t *out,
                       char *err, size_t err_cap);

/* 本地有界扫描。hits 容量 cap；返回写入 hits 的数量（<= cap），
 * total 若非空写入命中的总数（可能 > cap，用于"N 处命中"提示）。 */
int np_bi_search(const unsigned char *data, size_t len,
                 const np_bi_query_t *q,
                 np_bi_hit_t *hits, int cap, int *total);

#ifdef __cplusplus
}
#endif
#endif /* NP_BYTEINTEL_H */
