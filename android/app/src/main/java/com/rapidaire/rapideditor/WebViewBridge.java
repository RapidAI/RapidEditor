package com.rapidaire.rapideditor;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RelativeLayout;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.libsdl.app.SDLActivity;

/**
 * Embedded WebView for the editor's HTML preview pane.
 *
 * SDL draws the editor UI on a surface; a native WebView cannot live inside
 * that surface, so it is added as a sibling view on top of SDL's root layout
 * and positioned over the "wv-host" rectangle the CUI layout reserves. The
 * native side (np_webview_android.c) drives it through these static methods
 * and learns when the view is ready through nativeReady() so the preview can
 * switch from the system-browser fallback to the embedded pane.
 */
public final class WebViewBridge {
    private static final Handler UI = new Handler(Looper.getMainLooper());

    private static WebView view;
    private static int bx, by, bw, bh;
    private static boolean visible;

    private WebViewBridge() {}

    /* Implemented by np_webview_android.c; only flips a flag, the C callback
     * that refreshes the UI runs on the SDL thread in np_webview_ready(). */
    private static native void nativeReady(boolean ok);

    /** Create the WebView on the UI thread and attach it above SDL's surface. */
    public static void init() {
        UI.post(new Runnable() {
            @Override public void run() {
                try {
                    Activity a = SDLActivity.getContext() instanceof Activity
                            ? (Activity) SDLActivity.getContext() : null;
                    View rootView = SDLActivity.getContentView();
                    if (a == null || !(rootView instanceof ViewGroup)) {
                        nativeReady(false);
                        return;
                    }
                    ViewGroup root = (ViewGroup) rootView;
                    WebView w = new WebView(a);
                    w.setVisibility(View.GONE);
                    WebSettings st = w.getSettings();
                    st.setJavaScriptEnabled(true);
                    st.setDomStorageEnabled(true);
                    st.setLoadWithOverviewMode(true);
                    st.setUseWideViewPort(true);
                    /* Keep link clicks in the pane instead of launching a browser. */
                    w.setWebViewClient(new WebViewClient());
                    root.addView(w, new RelativeLayout.LayoutParams(0, 0));
                    view = w;
                    nativeReady(true);
                } catch (Throwable t) {
                    nativeReady(false);
                }
            }
        });
    }

    /** Position in physical pixels, top-left origin (from np_sync_webview). */
    public static void setBounds(final int x, final int y, final int w, final int h) {
        if (bx == x && by == y && bw == w && bh == h) return;
        bx = x; by = y; bw = w; bh = h;
        UI.post(new Runnable() {
            @Override public void run() {
                if (view == null) return;
                RelativeLayout.LayoutParams p = (RelativeLayout.LayoutParams) view.getLayoutParams();
                p.leftMargin = bx;
                p.topMargin = by;
                p.width = bw;
                p.height = bh;
                view.setLayoutParams(p);
            }
        });
    }

    public static void setVisible(final boolean show) {
        if (visible == show) return;
        visible = show;
        UI.post(new Runnable() {
            @Override public void run() {
                if (view != null) view.setVisibility(show ? View.VISIBLE : View.GONE);
            }
        });
    }

    public static void navigate(final String url) {
        if (url == null) return;
        UI.post(new Runnable() {
            @Override public void run() {
                if (view != null) view.loadUrl(url);
            }
        });
    }

    public static void reload() {
        UI.post(new Runnable() {
            @Override public void run() {
                if (view != null) view.reload();
            }
        });
    }

    public static void shutdown() {
        UI.post(new Runnable() {
            @Override public void run() {
                if (view != null) {
                    View rootView = SDLActivity.getContentView();
                    if (rootView instanceof ViewGroup) ((ViewGroup) rootView).removeView(view);
                    view.destroy();
                    view = null;
                }
            }
        });
    }
}
