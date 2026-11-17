#ifndef NP_WEBVIEW_H
#define NP_WEBVIEW_H

/* Cross-platform embedded WebView bridge.
 *
 * The editor core is C; each platform implements these functions in its
 * native language (C++ on Windows via WebView2, Objective-C on macOS via
 * WKWebView, C on Linux via WebKitGTK). The WebView is a native child view
 * positioned by np_webview_set_bounds() over a reserved pane rectangle.
 *
 * Runtime model: reuse the OS-provided web engine (Edge WebView2 / WebKit),
 * so no heavy runtime is bundled. Init is asynchronous: completion (or
 * failure) is reported through the callback with these event codes:
 *   0 = controller ready and usable
 *   1 = runtime missing / init failed (caller falls back to system browser)
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the embedded webview, bound to the top-level native window
 * (HWND on Windows, NSWindow* on macOS, XID as void* on Linux).
 * Returns 1 if init was started, 0 if it could not be started at all. */
int  np_webview_init(void *native_window, void (*cb)(int event));

/* True once the controller/view is created and usable. */
bool np_webview_ready(void);

/* Position/size the child view inside the parent, in PHYSICAL pixels. */
void np_webview_set_bounds(int x, int y, int w, int h);

/* Show/hide the child view. */
void np_webview_set_visible(bool visible);

/* Navigate to a URL (typically the built-in loopback HTTP server URL). */
void np_webview_navigate(const char *url);

/* Reload the current page. */
void np_webview_reload(void);

/* Release all native objects/child view. Safe to call repeatedly. */
void np_webview_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NP_WEBVIEW_H */
