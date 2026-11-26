/* test_h3h4_byteintel.c — H3 字节解释 + H4 结构化字节搜索单元测试. */
#include "np_byteintel.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static const np_bi_field_t *find_field(const np_bi_result_t *r, np_bi_type_t t) {
    for (int i = 0; i < r->nfields; i++) if (r->fields[i].type == t) return &r->fields[i];
    return NULL;
}

static int test_h3_ints(void) {
    printf("== H3: 整数/端序解释 ==\n");
    /* 0x03E8 = 1000 LE: E8 03 00 00 ; BE: 00 00 03 E8 */
    unsigned char le[] = { 0xE8, 0x03, 0x00, 0x00 };
    np_bi_result_t r;
    np_bi_interpret(le, sizeof le, 0, 4, &r);
    CHECK(r.nfields >= 8);
    const np_bi_field_t *f = find_field(&r, NP_BI_U16LE);
    CHECK(f && strcmp(f->value, "1000") == 0);
    CHECK(f && f->size == 2 && f->offset == 0);
    f = find_field(&r, NP_BI_U32LE);
    CHECK(f && strcmp(f->value, "1000") == 0);
    CHECK(f && f->size == 4);
    f = find_field(&r, NP_BI_U32BE);
    CHECK(f && strcmp(f->value, "3892510720") == 0); /* 0xE8030000 = 3892510720 */

    /* 有符号 -5 : 0xFB LE */
    unsigned char neg[] = { 0xFB };
    np_bi_interpret(neg, 1, 0, 1, &r);
    f = find_field(&r, NP_BI_I8);
    CHECK(f && strcmp(f->value, "-5") == 0);

    /* i32le -5 = FB FF FF FF */
    unsigned char neg32[] = { 0xFB, 0xFF, 0xFF, 0xFF };
    np_bi_interpret(neg32, 4, 0, 4, &r);
    f = find_field(&r, NP_BI_I32LE);
    CHECK(f && strcmp(f->value, "-5") == 0);
    return 0;
}

static int test_h3_float_ip(void) {
    printf("== H3: 浮点/IP/MAC/GUID/ASCII ==\n");
    /* f32le 1.5f = 00 00 C0 3F */
    unsigned char fl[] = { 0x00, 0x00, 0xC0, 0x3F };
    np_bi_result_t r;
    np_bi_interpret(fl, 4, 0, 4, &r);
    const np_bi_field_t *f = find_field(&r, NP_BI_F32LE);
    CHECK(f && f->plausible);
    CHECK(f && fabs(atof(f->value) - 1.5) < 1e-5);

    /* ipv4 192.168.1.1 */
    unsigned char ip[] = { 192, 168, 1, 1 };
    np_bi_interpret(ip, 4, 0, 4, &r);
    f = find_field(&r, NP_BI_IPV4);
    CHECK(f && strcmp(f->value, "192.168.1.1") == 0);

    /* ascii "Hi!" 全可打印 → plausible */
    np_bi_interpret((const unsigned char *)"Hi!", 3, 0, 3, &r);
    f = find_field(&r, NP_BI_ASCII);
    CHECK(f && f->plausible && strcmp(f->value, "Hi!") == 0);
    /* 含 NUL → ascii 不可打印 */
    unsigned char bin[] = { 'H', 0x00, 'i' };
    np_bi_interpret(bin, 3, 0, 3, &r);
    f = find_field(&r, NP_BI_ASCII);
    CHECK(f && !f->plausible);
    return 0;
}

static int test_h3_bounds(void) {
    printf("== H3: 越界/边界 ==\n");
    np_bi_result_t r;
    unsigned char d[] = { 1, 2, 3, 4 };
    np_bi_interpret(d, 4, 3, 100, &r);  /* length 钳制到 1 */
    CHECK(r.nfields >= 2);
    CHECK(find_field(&r, NP_BI_U8) != NULL);
    CHECK(find_field(&r, NP_BI_U32LE) == NULL);  /* 只剩 1 字节 */
    np_bi_interpret(d, 4, 4, 4, &r);    /* offset == len → 空 */
    CHECK(r.nfields == 0);
    np_bi_interpret(NULL, 0, 0, 0, &r); /* NULL → 空 */
    CHECK(r.nfields == 0);
    np_bi_interpret(d, 4, 0, 4, NULL);  /* NULL out 不崩 */
    return 0;
}

static int test_h4_parse(void) {
    printf("== H4: 查询解析 ==\n");
    np_bi_query_t q;
    char err[128];
    CHECK(np_bi_query_parse("u32le=1000", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_UINT_EQ && q.width == 4 && !q.big_endian && q.value == 1000);
    CHECK(strstr(q.desc, "u32le") != NULL);
    CHECK(np_bi_query_parse("u16be=0xDEAD", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_UINT_EQ && q.width == 2 && q.big_endian && q.value == 0xDEAD);
    CHECK(np_bi_query_parse("i32le=-5", &q, err, sizeof err));
    CHECK(q.signed_val && q.value == 0xFFFFFFFBu);
    CHECK(np_bi_query_parse("u32le>=256", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_UINT_GE && q.cmp == NP_BI_CMP_GE);
    CHECK(np_bi_query_parse("u8<=255", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_UINT_LE && q.width == 1);
    CHECK(np_bi_query_parse("PNG", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_SIG && q.sig[0] == (char)0x89 && q.sig[1] == 'P');
    CHECK(np_bi_query_parse("png:IHDR", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_CHUNK && strcmp(q.sig, "IHDR") == 0);
    CHECK(np_bi_query_parse("fill:0x00:16", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_FILL && q.fill_byte == 0 && q.min_run == 16);
    CHECK(np_bi_query_parse("fill:FF:8", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_FILL && q.fill_byte == 0xFF && q.min_run == 8);
    CHECK(np_bi_query_parse("str:hello", &q, err, sizeof err));
    CHECK(q.kind == NP_BIQ_STR && strcmp(q.str, "hello") == 0);
    /* 非法输入 */
    CHECK(!np_bi_query_parse("", &q, err, sizeof err));
    CHECK(!np_bi_query_parse("u24le=1", &q, err, sizeof err));
    CHECK(!np_bi_query_parse("u32le", &q, err, sizeof err));  /* 缺比较符 */
    CHECK(!np_bi_query_parse("u32le=xyz", &q, err, sizeof err));
    CHECK(!np_bi_query_parse("fill:GG:8", &q, err, sizeof err));
    CHECK(!np_bi_query_parse("bogus:thing", &q, err, sizeof err));
    CHECK(!np_bi_query_parse(NULL, &q, err, sizeof err));
    return 0;
}

static int test_h4_search_uint(void) {
    printf("== H4: 整数结构化搜索 ==\n");
    /* 三个值各占 4 字节，之间用 0xFF 填充（其滑动窗口值极大，避免 0x03 前导 0 产生小值误命中）。
       注意：u32le=1000 只在 0 与 8 精确匹配；滑动窗口若在别处凑出 1000 属正确行为，
       这里用 0xFF 间隔确保只有设计的位置命中。 */
    unsigned char d[20];
    memset(d, 0xFF, sizeof d);
    d[0]=0xE8; d[1]=0x03; d[2]=0; d[3]=0;      /* 1000 @0 */
    d[8]=0xE8; d[9]=0x03; d[10]=0; d[11]=0;    /* 1000 @8 */
    d[16]=0xD0; d[17]=0x07; d[18]=0; d[19]=0;  /* 2000 @16 */
    np_bi_query_t q; char err[128];
    np_bi_hit_t hits[16]; int total = 0;
    CHECK(np_bi_query_parse("u32le=1000", &q, err, sizeof err));
    int n = np_bi_search(d, sizeof d, &q, hits, 16, &total);
    CHECK(n == 2 && total == 2);
    CHECK(n > 0 && hits[0].offset == 0 && hits[0].length == 4);
    CHECK(n > 1 && hits[1].offset == 8);

    /* >= 1000 命中 3 个（1000,1000,2000），0xFF 间隔窗口均 > 1000 也会命中；
       所以用精确等值验证计数，>= 仅验证至少这三个位置都命中。 */
    CHECK(np_bi_query_parse("u32le>=1000", &q, err, sizeof err));
    total = 0; n = np_bi_search(d, sizeof d, &q, hits, 16, &total);
    {
        /* 17 个滑动窗口全部 >=1000（含 0xFF 填充），cap=16 只留前 16 个，
           故只校验前两个设计值与 total 上限语义。 */
        bool h0=false,h8=false;
        for (int i=0;i<n;i++){ if(hits[i].offset==0)h0=true; if(hits[i].offset==8)h8=true; }
        CHECK(h0 && h8 && total == 17);
    }

    /* cap 截断但 total 完整 */
    CHECK(np_bi_query_parse("u8>=0", &q, err, sizeof err));
    total = 0; n = np_bi_search(d, sizeof d, &q, hits, 5, &total);
    CHECK(n == 5 && total == 20);

    /* i32le -5 */
    unsigned char neg[4] = { 0xFB,0xFF,0xFF,0xFF };
    CHECK(np_bi_query_parse("i32le=-5", &q, err, sizeof err));
    total = 0; n = np_bi_search(neg, 4, &q, hits, 8, &total);
    CHECK(n == 1 && total == 1 && hits[0].offset == 0);
    return 0;
}

static int test_h4_patterns(void) {
    printf("== H4: 签名/块/填充/文本搜索 ==\n");
    char err[128];
    np_bi_query_t q;
    np_bi_hit_t hits[16]; int total = 0;
    /* PNG 签名在 0，IHDR 块在 8，一段长填充在 33..48 */
    unsigned char d[64] = {0};
    const unsigned char sig[] = { 0x89,'P','N','G','\r','\n',0x1A,'\n' };
    memcpy(d, sig, 8);
    memcpy(d + 8, "IHDR", 4);
    memset(d + 33, 0x00, 15);
    memcpy(d + 50, "hello", 5);

    CHECK(np_bi_query_parse("png", &q, err, sizeof err));
    total = 0; CHECK(np_bi_search(d, sizeof d, &q, hits, 16, &total) == 1);
    CHECK(total == 1 && hits[0].offset == 0 && hits[0].length == 8);

    CHECK(np_bi_query_parse("png:IHDR", &q, err, sizeof err));
    total = 0; CHECK(np_bi_search(d, sizeof d, &q, hits, 16, &total) == 1);
    CHECK(total == 1 && hits[0].offset == 8 && hits[0].length == 4);

    CHECK(np_bi_query_parse("fill:0x00:8", &q, err, sizeof err));
    total = 0; np_bi_search(d, sizeof d, &q, hits, 16, &total);
    /* d 初始为全 0，只有 sig/IHDR/fill/hello 处非零；12..47 是连续 36 字节 0，
       应作为一个 run 命中（起点 12，长度 36）。 */
    bool found_run = false;
    for (int i = 0; i < total && i < 16; i++)
        if (hits[i].offset == 12 && hits[i].length >= 8) found_run = true;
    CHECK(found_run);

    CHECK(np_bi_query_parse("str:hello", &q, err, sizeof err));
    total = 0; CHECK(np_bi_search(d, sizeof d, &q, hits, 16, &total) == 1);
    CHECK(total == 1 && hits[0].offset == 50 && hits[0].length == 5);

    /* 空数据/防御 */
    CHECK(np_bi_search(NULL, 0, &q, hits, 16, &total) == 0);
    return 0;
}

int main(void) {
    printf("H3/H4 字节智能单元测试\n");
    test_h3_ints();
    test_h3_float_ip();
    test_h3_bounds();
    test_h4_parse();
    test_h4_search_uint();
    test_h4_patterns();
    if (failures) { printf("\n%d failure(s)\n", failures); return 1; }
    printf("\nALL H3/H4 TESTS PASSED\n");
    return 0;
}
