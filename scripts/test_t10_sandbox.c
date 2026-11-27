/* test_t10_sandbox.c - T10 sandbox policy gate unit tests. */
#include "np_sandbox.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

static void test_url_host(void) {
    char h[128];
    CHECK(np_sb_url_host("https://github.com/RapidAI/x.zip", h, sizeof h));
    CHECK(strcmp(h, "github.com") == 0);
    CHECK(np_sb_url_host("http://mirror.corp.local:8080/pkg", h, sizeof h));
    CHECK(strcmp(h, "mirror.corp.local") == 0);
    /* fail-closed on malformed / userinfo / unsupported scheme */
    CHECK(!np_sb_url_host("ftp://example.com/x", h, sizeof h));
    CHECK(!np_sb_url_host("https://user@evil.com/x", h, sizeof h));
    CHECK(!np_sb_url_host("not a url", h, sizeof h));
    CHECK(!np_sb_url_host("https:///empty", h, sizeof h));
}

static void test_offline_policy(void) {
    np_sb_policy_t p; np_sb_policy_defaults(&p);
    CHECK(!p.offline_only);
    CHECK(np_sb_check_url(&p, "https://github.com/x") == NP_SB_ALLOW);

    p.offline_only = true;
    CHECK(np_sb_check_url(&p, "https://github.com/x") == NP_SB_DENY_OFFLINE);
    /* empty mirror list = deny everything */
    CHECK(np_sb_check_url(&p, "https://anything.local/x") == NP_SB_DENY_OFFLINE);
    snprintf(p.mirror_hosts, sizeof p.mirror_hosts, "%s", "mirror.corp.local,*.internal.corp");
    CHECK(np_sb_check_url(&p, "https://mirror.corp.local/pkg") == NP_SB_ALLOW);
    CHECK(np_sb_check_url(&p, "https://ci.internal.corp/pkg") == NP_SB_ALLOW);
    /* bare suffix parent must NOT match wildcard */
    CHECK(np_sb_check_url(&p, "https://internal.corp/pkg") == NP_SB_DENY_OFFLINE);
    /* malformed url still denied even against mirror */
    CHECK(np_sb_check_url(&p, "garbage") == NP_SB_DENY_ARG);
    CHECK(np_sb_check_url(&p, NULL) == NP_SB_DENY_ARG);
}

static void test_host_match(void) {
    CHECK(np_sb_host_match("a.example.com,*.corp.local", "a.example.com"));
    CHECK(np_sb_host_match("*.corp.local", "x.corp.local"));
    CHECK(!np_sb_host_match("*.corp.local", "corp.local"));  /* no bare parent */
    CHECK(!np_sb_host_match("", "x.corp.local"));
    CHECK(!np_sb_host_match("*.corp.local", "x.evil.com"));
}

static void test_write_policy(void) {
    np_sb_policy_t p; np_sb_policy_defaults(&p);
    /* empty allow-list = no writes */
    CHECK(np_sb_check_write(&p, "out.txt") == NP_SB_DENY_PATH);
    snprintf(p.write_allow, sizeof p.write_allow, "%s", "out/,build/");
    CHECK(np_sb_check_write(&p, "out/a.bin") == NP_SB_ALLOW);
    CHECK(np_sb_check_write(&p, "build/x") == NP_SB_ALLOW);
    CHECK(np_sb_check_write(&p, "secret/a") == NP_SB_DENY_PATH);
    /* traversal / absolute / drive always denied */
    CHECK(np_sb_check_write(&p, "out/../../etc/passwd") == NP_SB_DENY_TRAVERSAL);
    CHECK(np_sb_check_write(&p, "/tmp/x") == NP_SB_DENY_TRAVERSAL);
    CHECK(np_sb_check_write(&p, "C:\\Windows\\x") == NP_SB_DENY_TRAVERSAL);
    CHECK(np_sb_check_write(&p, NULL) == NP_SB_DENY_ARG);
}

static void test_tool_policy(void) {
    np_sb_policy_t p; np_sb_policy_defaults(&p);
    CHECK(np_sb_check_tool(&p, "objdump") == NP_SB_DENY_TOOL);  /* empty allow */
    snprintf(p.tool_allow, sizeof p.tool_allow, "%s", "objdump,llvm-objdump,ghidra");
    CHECK(np_sb_check_tool(&p, "objdump") == NP_SB_ALLOW);
    CHECK(np_sb_check_tool(&p, "OBJDUMP") == NP_SB_ALLOW);      /* case-insensitive */
    CHECK(np_sb_check_tool(&p, "evil.exe") == NP_SB_DENY_TOOL);
    /* path separators rejected: basename only */
    CHECK(np_sb_check_tool(&p, "/usr/bin/objdump") == NP_SB_DENY_TOOL);
    p.allow_external_tools = false;
    CHECK(np_sb_check_tool(&p, "objdump") == NP_SB_DENY_TOOL);
}

static void test_resources(void) {
    np_sb_policy_t p; np_sb_policy_defaults(&p);
    CHECK(np_sb_check_resources(&p, 9999, 99999, 9999) == NP_SB_ALLOW); /* no limits */
    p.max_wall_sec = 60; p.max_cpu_ms = 30000; p.max_mem_mb = 2048;
    CHECK(np_sb_check_resources(&p, 60, 30000, 2048) == NP_SB_ALLOW);
    CHECK(np_sb_check_resources(&p, 61, 100, 100) == NP_SB_DENY_RESOURCE);
    CHECK(np_sb_check_resources(&p, 10, 30001, 100) == NP_SB_DENY_RESOURCE);
    CHECK(np_sb_check_resources(&p, 10, 100, 2049) == NP_SB_DENY_RESOURCE);
    CHECK(np_sb_check_resources(NULL, 1, 1, 1) == NP_SB_DENY_ARG);
}

static bool g_apply_called;
static bool fake_apply(const np_sb_policy_t *p, void *proc, void *u) {
    (void)p; (void)proc; (void)u;
    g_apply_called = true;
    return true;
}
static void test_enforce(void) {
    np_sb_policy_t p; np_sb_policy_defaults(&p);
    g_apply_called = false;
    CHECK(np_sb_enforce(&p, NULL, NULL, NULL));       /* honest no-op */
    CHECK(!g_apply_called);
    CHECK(np_sb_enforce(&p, fake_apply, NULL, NULL));
    CHECK(g_apply_called);
    CHECK(!np_sb_enforce(NULL, fake_apply, NULL, NULL));
}

static void fake_clock(char *buf, size_t cap, void *u) {
    (void)u;
    snprintf(buf, cap, "2026-11-27T00:00:00Z");
}
static void test_audit(void) {
    np_sb_audit_t *a = np_sb_audit_create(3);
    CHECK(a != NULL);
    np_sb_audit_set_clock(a, fake_clock, NULL);
    np_sb_audit_add(a, "url", "https://evil.com", NP_SB_DENY_OFFLINE);
    np_sb_audit_add(a, "write", "../x", NP_SB_DENY_TRAVERSAL);
    CHECK(np_sb_audit_count(a) == 2);
    const np_sb_event_t *e = np_sb_audit_at(a, 0);
    CHECK(e && strcmp(e->action, "url") == 0);
    CHECK(e && e->verdict == NP_SB_DENY_OFFLINE);
    CHECK(e && strcmp(e->time_iso, "2026-11-27T00:00:00Z") == 0);
    /* ring wraps at cap */
    np_sb_audit_add(a, "tool", "evil", NP_SB_DENY_TOOL);
    np_sb_audit_add(a, "fetch", "x", NP_SB_ALLOW);
    CHECK(np_sb_audit_count(a) == 3);
    e = np_sb_audit_at(a, 0);
    CHECK(e && strcmp(e->action, "write") == 0);   /* oldest evicted */
    CHECK(np_sb_audit_at(a, 99) == NULL);
    np_sb_audit_destroy(a);
}

static void test_persistence(void) {
    np_sb_policy_t p; np_sb_policy_defaults(&p);
    p.offline_only = true;
    snprintf(p.mirror_hosts, sizeof p.mirror_hosts, "%s", "mirror.corp.local");
    snprintf(p.write_allow, sizeof p.write_allow, "%s", "out/");
    p.allow_external_tools = false;
    snprintf(p.tool_allow, sizeof p.tool_allow, "%s", "objdump");
    p.max_wall_sec = 30; p.max_cpu_ms = 20000; p.max_mem_mb = 1024;
    const char *path = "t10-sandbox-test.ini";
    CHECK(np_sb_save(&p, path));
    np_sb_policy_t q; np_sb_policy_defaults(&q);
    CHECK(np_sb_load(&q, path) == 8);
    CHECK(q.offline_only == p.offline_only);
    CHECK(strcmp(q.mirror_hosts, p.mirror_hosts) == 0);
    CHECK(strcmp(q.write_allow, p.write_allow) == 0);
    CHECK(q.allow_external_tools == false);
    CHECK(strcmp(q.tool_allow, "objdump") == 0);
    CHECK(q.max_wall_sec == 30 && q.max_cpu_ms == 20000 && q.max_mem_mb == 1024);
    remove(path);
}

int main(void) {
    test_url_host();
    test_offline_policy();
    test_host_match();
    test_write_policy();
    test_tool_policy();
    test_resources();
    test_enforce();
    test_audit();
    test_persistence();
    printf("test_t10_sandbox: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
