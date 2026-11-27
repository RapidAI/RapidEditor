/* np_sqlquery.h — F6: 结构化查询与采样分析（只读预览，需确认后执行）.
 *
 * 文档要求（F6 · S2 · P1）：看得到 schema，无法验证假设。自然语言 → 只读
 * SQL 预览，需确认后执行；采样行/空值率/直方图；大文件按页范围与行数上限截断。
 *
 * 安全红线：本模块<b>只读</b>，只生成受限 SELECT 预览与描述性统计，
 * 绝不执行写操作（UPDATE/DELETE/INSERT/DROP/PRAGMA writable 等）。SQL 生成器
 * 只接受白名单结构：SELECT 列 FROM 已知表 [WHERE 列 op 字面量] [LIMIT n]；
 * 表名/列名必须来自 np_db_parse 的 schema（防止注入未声明标识符）；字面量
 * 只接受数字/带引号串且做 SQL 转义；一切非法输入 fail-closed 拒绝。
 * "需确认后执行"：plan 结构带 confirmed 字段，未确认时 preview() 只返回
 * 文本计划不返回数据；真实执行由调用方在用户确认后显式调用 run()。
 *
 * 诚实边界：这是自研的受限 SELECT 子集解释器（走 np_sqlite 的只读 B-tree
 * 采样），不是完整 SQLite 引擎——不支持 JOIN/GROUP BY/ORDER BY/聚合函数
 * 的真实求值（直方图为空值率的粗略分桶）；不支持索引加速与事务；
 * 统计基于采样行而非全表时会在结果里标注 sampled=true。
 */
#ifndef NP_SQLQUERY_H
#define NP_SQLQUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "np_sqlite.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP_SQ_MAX_SQL      512
#define NP_SQ_MAX_IDENT    64
#define NP_SQ_MAX_LITERAL  128
#define NP_SQ_MAX_ROWS     50
#define NP_SQ_MAX_OUT      24576
#define NP_SQ_MAX_BUCKETS  8

typedef enum {
    NP_SQ_OK = 0,
    NP_SQ_ERR_ARG,
    NP_SQ_ERR_PARSE,        /* 自然语言/SQL 无法解析为白名单结构 */
    NP_SQ_ERR_DENIED,       /* 命中写操作/危险关键字/未声明标识符 */
    NP_SQ_ERR_UNCONFIRMED,  /* 未确认就尝试执行 */
    NP_SQ_ERR_NOFILE,       /* 文件读取失败 */
    NP_SQ_ERR_NOTSQLITE,    /* 非 SQLite 库 */
    NP_SQ_ERR_TRUNCATED     /* 触达页/行上限，结果不完整 */
} np_sq_status_t;
const char *np_sq_status_name(np_sq_status_t s);

typedef enum { NP_SQ_EQ = 0, NP_SQ_NE, NP_SQ_LT, NP_SQ_LE,
               NP_SQ_GT, NP_SQ_GE } np_sq_op_t;
const char *np_sq_op_name(np_sq_op_t op);

/* 受限 SELECT 计划（可被渲染为 SQL 文本供用户确认）。 */
typedef struct {
    char     table[NP_SQ_MAX_IDENT];
    int      nsel;                       /* 选择的列数；0 = 全部列 */
    char     cols[NP_DB_MAX_COLS][NP_SQ_MAX_IDENT];
    bool     has_where;
    char     where_col[NP_SQ_MAX_IDENT];
    np_sq_op_t where_op;
    char     where_lit[NP_SQ_MAX_LITERAL];
    bool     where_lit_is_num;
    size_t   limit;
    bool     confirmed;                  /* 用户确认后才可执行 */
    char     sql[NP_SQ_MAX_SQL];         /* 规范化后的只读 SQL 文本 */
} np_sq_plan_t;

/* 采样分析结果。 */
typedef struct {
    int      ncols;
    char     cols[NP_DB_MAX_COLS][NP_SQ_MAX_IDENT];
    size_t   rows_returned;
    size_t   rows_scanned;
    /* 每列的统计 */
    struct {
        size_t nulls;
        size_t nonnull;
        double  num_min, num_max, num_sum;
        size_t  num_count;
        char    top[NP_SQ_MAX_LITERAL];  /* 最常见文本值（采样内） */
        size_t  top_count;
    } stats[NP_DB_MAX_COLS];
    char     rows[NP_SQ_MAX_ROWS][NP_DB_MAX_COLS][NP_SQ_MAX_LITERAL];
    bool     sampled;       /* 仅采样部分行（触达 max_rows） */
    bool     partial;       /* B-tree 遍历不完整 */
} np_sq_result_t;

/* 从自然语言解析受限查询计划（如「显示 users 表里 age 大于 25 的 name」）。
 * 解析不出或命中写操作词即 fail-closed 拒绝。成功时生成规范化只读 SQL。 */
np_sq_status_t np_sq_parse_nl(const char *text, const np_db_schema_t *schema,
                              np_sq_plan_t *out);

/* 从显式 SQL 解析受限查询计划（仅接受白名单 SELECT 子集）。 */
np_sq_status_t np_sq_parse_sql(const char *sql, const np_db_schema_t *schema,
                               np_sq_plan_t *out);

/* 预览：把计划渲染成供人确认的 SQL + 说明（不读数据、不需 confirmed）。 */
int np_sq_preview(const np_sq_plan_t *plan, char *out, size_t cap);

/* 执行：读取 db 文件，按计划做只读采样与统计。必须 plan->confirmed == true。
 * max_rows 为采样上限（0 用默认 NP_SQ_MAX_ROWS）。 */
np_sq_status_t np_sq_run(const char *path, const np_sq_plan_t *plan,
                         size_t max_rows, np_sq_result_t *out);

/* 把结果渲染为 Markdown 表格 + 统计（含 provenance 与 sampled/partial 标注）。 */
int np_sq_render(const np_sq_plan_t *plan, const np_sq_result_t *res,
                 const char *source, char *out, size_t cap);

/* 数据质量要求：仅从 schema 推断不可得，需采样。对每列给空值率文本描述
 * （基于 run 的 stats）。写入 out 有界。 */
int np_sq_quality(const np_sq_result_t *res, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_SQLQUERY_H */
