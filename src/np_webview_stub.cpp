#include "np_webview.h"

/* No-op WebView backend. Used on Linux when WebKitGTK is not available and on
 * any other platform without a native implementation. init() reports failure
 * (event 1) so the HTML preview shows the system-browser fallback UI. */

extern "C" {

int np_webview_init(void *native_window, void (*cb)(int event)) {
    (void)native_window;
    if (cb) cb(1);
    return 0;
}
bool np_webview_ready(void) { return false; }
void np_webview_set_bounds(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }
void np_webview_set_visible(bool visible) { (void)visible; }
void np_webview_navigate(const char *url) { (void)url; }
void np_webview_reload(void) {}
void np_webview_shutdown(void) {}

} // extern "C"
