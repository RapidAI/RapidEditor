#include "np_webview.h"

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <string>

/* macOS embedded WebView backed by the system WKWebView (WebKit.framework,
 * part of macOS — no extra runtime to ship). The view is added as a subview of
 * the CUI window's content view and receives AppKit events through CUI's own
 * NSEvent loop, so no separate run loop is needed.
 *
 * CUI gives us bounds in PHYSICAL pixels with a top-left origin; AppKit views
 * use logical points with a bottom-left origin, so we convert in set_bounds. */

namespace {

WKWebView            *g_view      = nil;
NSWindow             *g_parent    = nil;
NSView               *g_host      = nil;
bool                  g_ready     = false;
void               (* g_callback)(int) = nullptr;
std::string           g_pending_url;
bool                  g_visible   = true;
int                   g_x = 0, g_y = 0, g_w = 0, g_h = 0;

NSString *utf8_to_ns(const char *s) {
    return s ? [NSString stringWithUTF8String:s] : @"";
}

/* Apply the last known physical-pixel rect (top-left origin) to the WKWebView,
 * converting to AppKit points and flipping Y against the host view height. */
static void apply_frame() {
    if (!g_view || !g_host) return;
    CGFloat scale = g_parent ? g_parent.backingScaleFactor : 1.0;
    if (scale <= 0) scale = 1.0;
    CGFloat h = g_host.bounds.size.height;               // logical points
    CGFloat x = (CGFloat)g_x / scale;
    CGFloat w = (CGFloat)g_w / scale;
    CGFloat hh = (CGFloat)g_h / scale;
    CGFloat y = h - (CGFloat)g_y / scale - hh;           // flip to bottom-left
    [g_view setFrame:NSMakeRect(x, y, w, hh)];
    [g_view setHidden:(g_visible ? NO : YES)];
}

static void navigate_now(const std::string &url) {
    if (!g_view || url.empty()) return;
    NSURL *nsurl = [NSURL URLWithString:utf8_to_ns(url.c_str())];
    if (!nsurl) return;
    [g_view loadRequest:[NSURLRequest requestWithURL:nsurl]];
}

} // namespace

extern "C" {

int np_webview_init(void *native_window, void (*cb)(int event)) {
    if (g_view) return 1;
    g_callback = cb;
    g_parent = (NSWindow *)native_window;
    if (!g_parent) { if (cb) cb(1); return 0; }

    g_host = g_parent.contentView;
    if (!g_host) { if (cb) cb(1); return 0; }

    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    g_view = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:cfg];
    [g_view setAutoresizingMask:NSViewNotSizable];       // we position it exactly
    [g_host addSubview:g_view positioned:NSWindowAbove relativeTo:nil];
    [g_view setHidden:YES];

    g_ready = true;
    if (!g_pending_url.empty()) navigate_now(g_pending_url);
    if (cb) cb(0);
    return 1;
}

bool np_webview_ready(void) { return g_ready && g_view != nil; }

void np_webview_set_bounds(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
    apply_frame();
}

void np_webview_set_visible(bool visible) {
    g_visible = visible;
    if (g_view) [g_view setHidden:(visible ? NO : YES)];
}

void np_webview_navigate(const char *url) {
    if (!url) return;
    g_pending_url = url;
    if (g_ready) navigate_now(g_pending_url);
}

void np_webview_reload(void) {
    if (g_view) [g_view reload:nil];
}

void np_webview_shutdown(void) {
    if (g_view) {
        [g_view removeFromSuperview];
        g_view = nil;
    }
    g_host = nil;
    g_parent = nil;
    g_ready = false;
    g_callback = nullptr;
    g_pending_url.clear();
}

} // extern "C"

#endif /* __APPLE__ */
