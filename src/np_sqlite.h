/* np_sqlite.h — M1/F3: SQLite 数据库 schema 只读提取.
 *
 * 文档要求（F3 · S1 · P0）：SQLite 头 + B-tree / schema 表解析；
 * 表/列/类型/索引/行数/WAL 状态。
 *
 * 安全红线：本模块<b>只读解析</b>，绝不修改任何字节；写入需求一律走
 * F8（S2 的结构化 writer），H8 禁写策略兜底。
 * 行数为 B-tree 叶子页 cell 精确计数（非全表内容扫描）。
 * 只支持 UTF-8 编码库的 CREATE TABLE 列解析；UTF-16 库仍可列出对象但
 * 列解析标记 partially_parsed。
 * 不支持：溢出页链死循环防御外的畸形库容错、WITHOUT ROWID 跳表
 * （index 型 b-tree，遇到即标 partially_parsed）。
 */
#ifndef NP_SQLITE_H
#define NP_SQLITE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_DB_MAX_TABLES  16
#define NP_DB_MAX_COLS    32
#define NP_DB_MAX_OBJECTS 32
#define NP_DB_MAX_NAME    64
#define NP_DB_MAX_SQL     512

typedef enum {
    NP_DB_OK = 0,
    NP_DB_ERR_ARG,
    NP_DB_ERR_NOT_SQLITE,     /* 魔数不符（含加密/其他格式） */
    NP_DB_ERR_TOO_SMALL,      /* 不足 16 字节 */
    NP_DB_ERR_CORRUPT         /* 头字段非法（页大小等） */
} np_db_status_t;

const char *np_db_status_name(np_db_status_t s);

/* 索引 / 视图 / 触发器等非表对象 */
typedef struct {
    char     type[12];      /* "index" / "view" / "trigger" */
    char     name[NP_DB_MAX_NAME];
    char     tbl[NP_DB_MAX_NAME];
    uint32_t rootpage;
    char     sql[NP_DB_MAX_SQL];
} np_db_object_t;

typedef struct {
    char name[NP_DB_MAX_NAME];
    char decl_type[24];
    bool notnull;
    bool pk;
} np_db_col_t;

typedef struct {
    char       name[NP_DB_MAX_NAME];
    char       sql[NP_DB_MAX_SQL];
    uint32_t   rootpage;
    uint64_t   est_rows;        /* 叶子页 cell 精确计数 */
    int        ncols;
    np_db_col_t cols[NP_DB_MAX_COLS];
    bool       rowid_alias;     /* 单列 INTEGER PRIMARY KEY */
    bool       truncated;       /* sql 被截断或列超上限 */
} np_db_table_t;

typedef struct {
    bool     valid;
    bool     wal;               /* read/write version == 2 */
    bool     encrypted;         /* 预留：文件级加密特征（当前=魔数不符即 NOT_SQLITE） */
    bool     partially_parsed;  /* b-tree 遍历中断/溢出/超限 */
    uint32_t page_size;         /* 512..65536（65536 用值 1 编码在头中） */
    uint32_t n_pages;           /* 头部声明 */
    uint32_t freelist_pages;
    uint8_t  text_encoding;     /* 1=UTF8, 2=UTF16LE, 3=UTF16BE */
    int      ntables;
    np_db_table_t tables[NP_DB_MAX_TABLES];
    int      nobjects;
    np_db_object_t objects[NP_DB_MAX_OBJECTS];
} np_db_schema_t;

/* 主入口：对内存缓冲做只读 schema 提取。data==NULL 或 len<16 → 错误码。 */
np_db_status_t np_db_parse(const unsigned char *data, size_t len,
                           np_db_schema_t *out);

/* 按名字查表；未找到返回 NULL。 */
const np_db_table_t *np_db_find_table(const np_db_schema_t *s, const char *name);

/* ------------------------------------------------------------------ */
/* F6: 只读行采样（复用 B-tree 遍历；绝不修改字节）                     */
/* ------------------------------------------------------------------ */

#define NP_DB_MAX_SAMPLE_COLS 32
#define NP_DB_MAX_CELL_TEXT   200

/* 单个单元格的文本化表示（有界，NULL 用 is_null 标记）。 */
typedef struct {
    char     text[NP_DB_MAX_CELL_TEXT];
    bool     is_null;
    uint64_t ival;              /* 整数 serial 的原始值 */
    bool     is_int;            /* serial 1..6/8/9 */
    bool     is_blob;           /* serial>=12 且奇数 */
    bool     truncated;         /* 文本被截断 */
} np_db_cell_t;

/* 一行采样：cells[0..ncells-1] 对应表列顺序（不足列数为 NULL）。 */
typedef struct {
    uint64_t rowid;
    int      ncells;
    np_db_cell_t cells[NP_DB_MAX_SAMPLE_COLS];
} np_db_row_t;

/* 采样回调：返回 false 停止遍历。row 指向的数据仅在回调内有效。 */
typedef bool (*np_db_row_cb)(const np_db_row_t *row, void *user);

/* 对已 np_db_parse 的库缓冲按表名做只读行采样，最多 max_rows 行。
 * 返回实际访问行数；partial 置位表示遍历未完整（页上限/坏页/超限）。
 * 数据所有权仍在调用方，本函数不修改 data。 */
size_t np_db_sample_rows(const unsigned char *data, size_t len,
                         const np_db_schema_t *schema, const char *table,
                         size_t max_rows, np_db_row_cb cb, void *user,
                         bool *partial);

#ifdef __cplusplus
}
#endif
#endif /* NP_SQLITE_H */
