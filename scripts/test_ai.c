#include "../src/np_ai.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

int main(void) {
    char out[512];
    CHECK(np_ai_subst("claude -p {file} > {out}", "{file}", "\"C:\\a b\\ctx.txt\"", out, sizeof out), "subst1 ok");
    CHECK(strstr(out, "\"C:\\a b\\ctx.txt\"") && strstr(out, "{out}") != 0, "first placeholder replaced, second untouched");

    char out2[512];
    CHECK(np_ai_subst("claude -p {file} > {out}", "{out}", "\"C:\\res.txt\"", out2, sizeof out2), "subst2 ok");
    CHECK(strstr(out2, "{file}") && strstr(out2, "\"C:\\res.txt\""), "second placeholder replaced");

    /* Both, sequential. */
    char a[512], b[512];
    np_ai_subst("codex exec {file} --out {out}", "{file}", "F", a, sizeof a);
    np_ai_subst(a, "{out}", "O", b, sizeof b);
    CHECK(strcmp(b, "codex exec F --out O") == 0, "both replaced sequentially");

    /* Too small. */
    CHECK(!np_ai_subst("aaaaaaaaaa{file}", "{file}", "XXXXXXXXXXXXXX", out, 8), "small buffer rejected");

    /* write/read context file. */
    const char *p = "test-ai-ctx.tmp";
    CHECK(np_ai_write_context(p, "hello AI 中文"), "write context ok");
    char rb[128];
    long n = np_ai_read_result(p, rb, sizeof rb);
    CHECK(n > 0 && strcmp(rb, "hello AI 中文") == 0, "read context back utf-8");
    remove(p);

#ifdef _WIN32
    /* CJK path round-trip: execute a command whose output path has Chinese
     * characters. This used to fail with CreateProcessA (ANSI code page). */
    {
        const char *ctx = "测试-上下文.tmp";
        const char *outp = "测试-结果-中文.tmp";
        CHECK(np_ai_write_context(ctx, "laTeX 公式 $x^2$"), "write CJK-named context");
        const char *tpl = "echo agent-ok > {out}";
        char err[256];
        bool ok = np_ai_exec_timeout(tpl, ctx, outp, NULL, 5000, err, sizeof err);
        CHECK(ok, "exec with CJK output path succeeds");
        char res[128];
        long rn = np_ai_read_result(outp, res, sizeof res);
        CHECK(rn > 0 && strstr(res, "agent-ok"), "CJK path result read back");
        remove(ctx); remove(outp);
    }
    /* Timeout: a sleeping command must be killed and must NOT inherit a stale
     * result. Pre-create a result file to confirm it is removed first. */
    {
        const char *ctx = "test-timeout-ctx.tmp";
        const char *outp = "test-timeout-out.tmp";
        np_ai_write_context(ctx, "x");
        FILE *stale = fopen(outp, "wb");
        if (stale) { fputs("STALE", stale); fclose(stale); }
        const char *tpl = "ping -n 30 127.0.0.1 > nul & echo late > {out}";
        char err[256];
        bool ok = np_ai_exec_timeout(tpl, ctx, outp, NULL, 800, err, sizeof err);
        CHECK(!ok, "hung command reported failure on timeout");
        CHECK(strstr(err, "超时") != NULL, "timeout error message set");
        FILE *chk = fopen(outp, "rb");
        CHECK(chk == NULL, "stale result removed, late output not written");
        if (chk) fclose(chk);
        remove(ctx); remove(outp);
    }
#endif

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
