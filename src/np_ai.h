#ifndef NP_AI_H
#define NP_AI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

/* Minimal helper for calling a locally-installed coding agent (Codex,
 * Claude Code, aider, ...) from a worker thread. The agent command is a
 * user-editable template with two placeholders:
 *   {file}  -> quoted path of the UTF-8 context+instruction file
 *   {out}   -> quoted path of the result file the agent writes to (stdout)
 * The placeholders are substituted and the whole line is executed via the
 * shell, with stdout/stderr redirected into {out}. */

/* Write `text` (UTF-8) to `path`; returns true on success. */
bool np_ai_write_context(const char *path, const char *text);

/* Substitute {file}/{out} placeholders in `tpl` into `cmd`, then run it.
 * On Windows the console window is hidden (CREATE_NO_WINDOW). Returns true
 * when the command produced a non-empty result file. `err` receives a short
 * human-readable message on failure. */
bool np_ai_exec(const char *tpl, const char *ctx_path, const char *out_path,
                const char *sys, char *err, size_t err_cap);

/* Same as np_ai_exec but with an explicit timeout in milliseconds (Windows).
 * Used by tests with a short timeout; pass 0 for the default. */
bool np_ai_exec_timeout(const char *tpl, const char *ctx_path, const char *out_path,
                        const char *sys, int timeout_ms, char *err, size_t err_cap);

/* Read the result file into `buf` (UTF-8). Returns bytes read, or -1. */
long np_ai_read_result(const char *path, char *buf, size_t cap);

/* Replace every occurrence of `needle` in `s` with `repl`, into `out`.
 * Returns false if out is too small. */
bool np_ai_subst(const char *s, const char *needle, const char *repl,
                 char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
