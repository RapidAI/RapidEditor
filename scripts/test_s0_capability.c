/* Unit tests for X1+X2 (np_capability): registry + routing budget. */
#include "np_capability.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static bool reg(np_registry_t *r, const char *id, np_tool_kind_t kind, bool core,
                int budget, const char *desc) {
    np_tool_desc_t d; memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", id);
    d.kind = kind; d.core = core; d.budget = budget;
    d.health = true; d.authorized = true; d.risk = NP_RISK_READONLY;
    snprintf(d.description, sizeof(d.description), "%s", desc);
    return np_registry_register(r, &d);
}

int main(void) {
    np_registry_t *r = np_registry_create();
    CHECK(r && np_registry_count(r) == 0);

    /* core resident tools */
    CHECK(reg(r, "editor.text.read", NP_TOOL_NATIVE, true, 1, "read text lines and selection"));
    CHECK(reg(r, "editor.binary.read", NP_TOOL_NATIVE, true, 1, "read binary byte window and disassembly"));
    CHECK(reg(r, "knowledge.search", NP_TOOL_SEARCH, true, 2, "search knowledge base"));
    /* low-frequency tools */
    CHECK(reg(r, "capability.discover", NP_TOOL_NATIVE, true, 1, "discover more capabilities"));
    CHECK(reg(r, "ghidra.decompile", NP_TOOL_SKILL, false, 8, "decompile function to pseudocode with ghidra"));
    CHECK(reg(r, "mcp.enterprise.kb", NP_TOOL_MCP, false, 4, "enterprise knowledge via mcp"));
    CHECK(np_registry_count(r) == 6);

    /* eligibility filters */
    const np_tool_desc_t *d = np_registry_get(r, "ghidra.decompile");
    CHECK(d && np_tool_eligible(d, 10));
    CHECK(!np_tool_eligible(d, 4));   /* budget 8 > cap 4 */
    np_registry_set_health(r, "mcp.enterprise.kb", false);
    CHECK(!np_tool_eligible(np_registry_get(r, "mcp.enterprise.kb"), 100));
    np_registry_set_health(r, "mcp.enterprise.kb", true);
    np_registry_set_authorized(r, "mcp.enterprise.kb", false);
    CHECK(!np_tool_eligible(np_registry_get(r, "mcp.enterprise.kb"), 100));
    np_registry_set_authorized(r, "mcp.enterprise.kb", true);

    /* relevance */
    CHECK(np_tool_relevance(np_registry_get(r, "ghidra.decompile"), "decompile function") > 0);
    CHECK(np_tool_relevance(np_registry_get(r, "ghidra.decompile"), "read text lines") == 0);

    /* routing: core tools always resident, then ranked non-core */
    char ids[16][80];
    size_t n = np_registry_route(r, "decompile function", 100, 6, ids, 16);
    CHECK(n == 6);
    /* first four are core (insertion order): text, binary, knowledge, discover */
    CHECK(strcmp(ids[0], "editor.text.read") == 0);
    CHECK(strcmp(ids[1], "editor.binary.read") == 0);
    CHECK(strcmp(ids[2], "knowledge.search") == 0);
    CHECK(strcmp(ids[3], "capability.discover") == 0);
    /* ghidra should outrank mcp for a "decompile" query */
    CHECK(strcmp(ids[4], "ghidra.decompile") == 0);
    CHECK(strcmp(ids[5], "mcp.enterprise.kb") == 0);

    /* hard cap on tool count */
    n = np_registry_route(r, "anything", 100, 3, ids, 16);
    CHECK(n == 3);

    /* failure downrank -> auto-disable after 3 consecutive failures */
    np_registry_report(r, "ghidra.decompile", false);
    np_registry_report(r, "ghidra.decompile", false);
    CHECK(np_tool_eligible(np_registry_get(r, "ghidra.decompile"), 100));
    np_registry_report(r, "ghidra.decompile", false);
    CHECK(!np_tool_eligible(np_registry_get(r, "ghidra.decompile"), 100));
    np_registry_set_health(r, "ghidra.decompile", true); /* health check clears streak */
    CHECK(np_tool_eligible(np_registry_get(r, "ghidra.decompile"), 100));

    /* removal */
    CHECK(np_registry_remove(r, "mcp.enterprise.kb"));
    CHECK(np_registry_count(r) == 5);
    CHECK(np_registry_get(r, "mcp.enterprise.kb") == NULL);

    np_registry_destroy(r);
    printf("test_s0_capability: ALL PASSED\n");
    return 0;
}
