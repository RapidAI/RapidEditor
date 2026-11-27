/* np_featcard.h — F4: AI 特征抽取与特征卡.
 *
 * 文档要求（F4 · S2 · P1）：结构读出来了，但"这份文档/库在讲什么"仍要人工通读。
 * 文档主题/实体/表格语义/异常段落；DB 列语义/PII 标记/外键推断/数据质量；
 * 每条特征带证据与置信度。
 *
 * 安全红线：本模块<b>只读</b>，绝不修改源文件；从 F2 (np_ooxml) / F3 (np_sqlite)
 * 的只读解析结果抽取特征，不重新实现 ZIP/XML/B-tree 解码。每条特征必须带可回跳
 * 证据（file#结构路径 或 file:0xOFFSET），无证据一律拒绝（fail-closed）；低于
 * 置信度阈值的推断不进卡；PII 形态的值在入卡前强制脱敏。主题/列语义/外键属
 * AI 候选（L1/L2），随卡标注证据等级，不与确定性事实（L0）混淆。
 *
 * 诚实边界：主题词频与列语义是启发式，不是 NLP 模型；外键仅按命名约定推断，
 * 不解析真实约束；数据质量只能从 schema 层（NOT NULL / PK / WAL）推断，
 * 不扫描真实行值，因此"空值率"不在本模块范围（属 F6）。
 */
#ifndef NP_FEATCard_H
#define NP_FEATCard_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_FC_MAX_ID       192
#define NP_FC_MAX_NAME     80
#define NP_FC_MAX_VALUE    200
#define NP_FC_MAX_EVIDENCE 256
#define NP_FC_MAX_SOURCE   200
#define NP_FC_MAX_FEATURES 128
#define NP_FC_MAX_CARD     24576
#define NP_FC_MIN_CONF     25          /* 低于此值不进卡 */
#define NP_FC_MAX_TEXT     4096        /* 主题词频扫描上限 */

typedef enum {
    NP_FC_OK = 0,
    NP_FC_ERR_ARG,
    NP_FC_ERR_FULL,
    NP_FC_ERR_DUP,
    NP_FC_ERR_LOW_CONF,
    NP_FC_ERR_NO_EVIDENCE,
    NP_FC_ERR_PARSE,
    NP_FC_ERR_UNSUPPORTED
} np_fc_status_t;

const char *np_fc_status_name(np_fc_status_t s);

typedef enum { NP_FC_DOC = 0, NP_FC_DB } np_fc_domain_t;
const char *np_fc_domain_name(np_fc_domain_t d);

/* 特征类别：格式/结构/语义/质量/安全 */
typedef enum {
    NP_FC_FORMAT = 0,
    NP_FC_STRUCTURE,
    NP_FC_SEMANTIC,
    NP_FC_QUALITY,
    NP_FC_SECURITY
} np_fc_kind_t;
const char *np_fc_kind_name(np_fc_kind_t k);

/* 证据等级：L0 确定性（解析器事实）、L1 工具推导（命名/结构约定）、
 * L2 AI 候选（启发式推断，需人工确认）。 */
typedef enum { NP_FC_L0 = 0, NP_FC_L1, NP_FC_L2 } np_fc_level_t;
const char *np_fc_level_name(np_fc_level_t l);

typedef struct {
    np_fc_kind_t  kind;
    np_fc_level_t level;
    char     id[NP_FC_MAX_ID];
    char     name[NP_FC_MAX_NAME];
    char     value[NP_FC_MAX_VALUE];
    char     evidence[NP_FC_MAX_EVIDENCE];
    int      conf;                       /* 0..100 */
} np_fc_feature_t;

typedef struct {
    np_fc_domain_t domain;
    char     source[NP_FC_MAX_SOURCE];   /* 引用协议中的文件名 */
    int      n;
    np_fc_feature_t items[NP_FC_MAX_FEATURES];
    int      rejected_low_conf;
    int      rejected_no_evidence;
    int      rejected_dup;
    int      redacted;
    bool     truncated;
} np_fc_card_t;

/* 初始化空卡（全零 + domain/source）。 */
void np_fc_init(np_fc_card_t *c, np_fc_domain_t domain, const char *source);

/* 统一入卡门禁：id 非空且唯一、evidence 含 '#' 或 ':0x'、conf>=阈值、
 * kind/level/conf 合法；value 中的 PII 形态会被自动脱敏。返回拒绝原因。 */
np_fc_status_t np_fc_add(np_fc_card_t *c, np_fc_kind_t kind, np_fc_level_t level,
                         const char *id, const char *name, const char *value,
                         const char *evidence, int conf);

/* 按 id 精确查找；不存在返回 NULL。 */
const np_fc_feature_t *np_fc_find(const np_fc_card_t *c, const char *id);

/* 渲染为 Markdown 特征卡（含 provenance 头与统计）。返回写入字节数，
 * 容量不足返回 -1（不产出半张卡）。 */
int np_fc_render(const np_fc_card_t *c, char *out, size_t cap);

/* PII 形态检测（与 X9/X11 同口径）：邮箱/身份证/手机号/密钥/长数字。
 * 命中时 reason 指向可读原因（可为 NULL）。 */
bool np_fc_looks_like_pii(const char *s, const char **reason);
/* 脱敏到 out（有界）。 */
void np_fc_redact(const char *in, char *out, size_t cap);

/* 从 F2/F3 的只读解析结果抽取特征卡。path 为磁盘文件路径，source 为
 * 引用协议使用的文件名（如 "golden.docx"），用于构造证据回跳串。 */
np_fc_status_t np_fc_extract_ooxml(const char *path, const char *source,
                                   np_fc_card_t *out);
np_fc_status_t np_fc_extract_sqlite(const char *path, const char *source,
                                    np_fc_card_t *out);

#ifdef __cplusplus
}
#endif
#endif /* NP_FEATCard_H */
