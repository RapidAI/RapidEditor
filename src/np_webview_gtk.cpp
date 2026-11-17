#include "np_webview.h"

#ifdef __linux__

/* Linux embedded WebView backed by WebKitGTK (GTK3 + webkit2gtk-4.1).
 *
 * CUI owns a bare X11 window and drives its own select() event loop, while
 * WebKitGTK is a GTK widget needing the GLib/GTK main context. We bridge them:
 *   - create a borderless GtkWindow holding the WebKitWebView,
 *   - reparent that window's XID under CUI's X11 window via XReparentWindow,
 *   - pump GTK events from the bridge each frame (gtk_main_iteration_do),
 * so no separate GTK thread or full gtk_main() is required.
 *
 * Coordinates from CUI are physical pixels, top-left origin — the same space
 * X11 uses, so no conversion is needed.
 */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

#include <string>

namespace {

GtkWidget *g_toplevel = nullptr;     /* borderless GtkWindow reparented into CUI */
GtkWidget *g_webview  = nullptr;     /* WebKitWebView */
Window     g_parent_xid = 0;
bool       g_ready     = false;
bool       g_visible   = true;
int        g_x=0, g_y=0, g_w=0, g_h=0;
void    (* g_callback)(int) = nullptr;
std::string g_pending_url;

bool gdk_ready() {
    static bool inited = false;
    if (inited) return true;
    int argc = 0;
    char **argv = nullptr;
    inited = gtk_init_check(&argc, &argv) != 0;
    return inited;
}

void pump() {
    while (gtk_events_pending()) gtk_main_iteration();
}

void place() {
    if (!g_toplevel) return;
    GdkWindow *gw = gtk_widget_get_window(g_toplevel);
    if (!gw) return;
    Window child = gdk_x11_window_get_xid(gw);
    Display *dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());
    if (!dpy || !child) return;
    if (g_visible && g_w > 0 && g_h > 0) {
        XMoveResizeWindow(dpy, child, g_x, g_y,
                          (unsigned)g_w, (unsigned)g_h);
        XMapWindow(dpy, child);
    } else {
        XUnmapWindow(dpy, child);
    }
    XFlush(dpy);
}

} // namespace

extern "C" {

int np_webview_init(void *native_window, void (*cb)(int event)) {
    if (g_webview) return 1;
    g_callback = cb;
    if (!native_window || !gdk_ready()) {
        if (cb) cb(1);
        return 0;
    }
    g_parent_xid = (Window)(uintptr_t)native_window;

    g_toplevel = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(g_toplevel), FALSE);
    gtk_widget_set_app_paintable(g_toplevel, TRUE);

    g_webview = webkit_web_view_new();
    gtk_container_add(GTK_CONTAINER(g_toplevel), g_webview);
    gtk_widget_show(g_webview);
    gtk_widget_show_all(g_toplevel);       /* realize -> GdkWindow/XID exists */

    /* Reparent the GTK window under CUI's X11 window. */
    pump();
    GdkWindow *gw = gtk_widget_get_window(g_toplevel);
    if (!gw) { if (cb) cb(1); return 0; }
    Window child = gdk_x11_window_get_xid(gw);
    Display *dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());
    if (!dpy || !child) { if (cb) cb(1); return 0; }
    XReparentWindow(dpy, child, g_parent_xid, 0, 0);
    XFlush(dpy);

    g_ready = true;
    if (!g_pending_url.empty())
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(g_webview), g_pending_url.c_str());
    place();
    if (cb) cb(0);
    return 1;
}

bool np_webview_ready(void) { return g_ready && g_webview != nullptr; }

void np_webview_set_bounds(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
    place();
    pump();
}

void np_webview_set_visible(bool visible) {
    g_visible = visible;
    place();
    pump();
}

void np_webview_navigate(const char *url) {
    if (!url) return;
    g_pending_url = url;
    if (g_ready && g_webview)
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(g_webview), url);
}

void np_webview_reload(void) {
    if (g_webview) webkit_web_view_reload(WEBKIT_WEB_VIEW(g_webview));
}

void np_webview_shutdown(void) {
    if (g_toplevel) {
        gtk_widget_destroy(g_toplevel);
        g_toplevel = nullptr;
    }
    g_webview = nullptr;
    g_ready = false;
    g_callback = nullptr;
    g_pending_url.clear();
}

} // extern "C"

#endif /* __linux__ */
