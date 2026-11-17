#ifndef NP_WEBVIEW2_BRIDGE_H
#define NP_WEBVIEW2_BRIDGE_H

/* Win32 WebView2 bridge. The implementation is C++ (WebView2 is a C++ COM
 * API), but this header is plain C so the editor core can call it without
 * pulling C++ into its translation units.
 *
 * Runtime model: Evergreen. We load WebView2Loader.dll (shipped next to the
 * executable) and reuse the WebView2 Runtime already installed with Edge on
 * modern Windows. No runtime is bundled; if it is missing, np_webview2_init()
 * returns 0 and the caller falls back to the system browser.
 *
 * The WebView is a child HWND of the editor's top-level window. The caller
 * positions it with np_webview2_set_bounds() whenever the preview pane moves
 * or resizes, and hides it when the pane is folded.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the WebView2 environment and controller asynchronously.
 * hwnd        : parent (top-level editor) window.
 * eventHandler: optional pointer to a function receiving this event code:
 *                0 = ready, 1 = runtime missing/failed, 2 = controller ready.
 * Returns 1 if initialisation was started, 0 if the loader/runtime is not
 * available (caller should fall back). Completion is reported via cb. */
int np_webview2_init(void *hwnd, void (*cb)(int event));

/* Returns 1 once the controller is created and usable. */
bool np_webview2_ready(void);

/* Position/size the child WebView inside the parent, in physical pixels. */
void np_webview2_set_bounds(int x, int y, int w, int h);

/* Show/hide the child WebView. */
void np_webview2_set_visible(bool visible);

/* Navigate to a URL (typically the built-in loopback HTTP server URL). */
void np_webview2_navigate(const char *url);

/* Reload the current page. */
void np_webview2_reload(void);

/* Process a Win32 message. Call from the top-level window procedure. Return
 * true if the message was handled by the bridge. */
bool np_webview2_handle_message(void *hwnd, unsigned int msg, void *wp, void *lp, long *result);

/* Release all COM objects/child window. Safe to call repeatedly. */
void np_webview2_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NP_WEBVIEW2_BRIDGE_H */
