package com.rapidaire.rapideditor;

import android.content.Intent;
import android.net.Uri;

import androidx.core.content.FileProvider;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * In-app update helper driven over JNI by the native editor (np_updater.c).
 *
 * Network and installer work runs on worker threads; the C side polls the
 * integer states each frame and renders its own progress dialog. Results are
 * plain static fields - the flow is one check / one download at a time.
 *
 * States follow np_updater.h: check 0 idle 1 running 2 ok 3 failed,
 * download 0 idle 1 running 2 done 3 failed.
 */
public class Updater {

    private static final String RELEASE_URL =
            "https://api.github.com/repos/RapidAI/RapidEditor/releases/latest";

    public static final int CHECK_IDLE = 0, CHECK_RUNNING = 1, CHECK_OK = 2, CHECK_FAILED = 3;
    public static final int DL_IDLE = 0, DL_RUNNING = 1, DL_DONE = 2, DL_FAILED = 3;

    private static volatile int checkState = CHECK_IDLE;
    private static volatile int dlState = DL_IDLE;
    private static volatile int progress = 0;

    private static volatile String body = null;
    private static volatile String error = "";

    /* ---- check -------------------------------------------------------- */

    /** Fetch the latest release JSON on a worker thread. */
    public static void checkAsync() {
        if (checkState == CHECK_RUNNING) return;
        checkState = CHECK_RUNNING;
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    body = httpGet(RELEASE_URL);
                    checkState = (body != null && body.length() > 0) ? CHECK_OK : CHECK_FAILED;
                    if (checkState == CHECK_FAILED) error = "empty response from GitHub";
                } catch (Exception e) {
                    error = String.valueOf(e);
                    checkState = CHECK_FAILED;
                }
            }
        }).start();
    }

    public static int getCheckState() { return checkState; }

    /** Full releases/latest JSON, consumed once by the native parser. */
    public static String getBody() { return body; }

    public static String getError() { return error; }

    /* ---- download ----------------------------------------------------- */

    /** Stream url to dest on a worker thread, reporting 0..100 progress. */
    public static void startDownload(final String url, final String dest) {
        if (dlState == DL_RUNNING) return;
        dlState = DL_RUNNING;
        progress = 0;
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    HttpURLConnection c = open(url);
                    long total = c.getContentLength();
                    InputStream in = c.getInputStream();
                    File tmp = new File(dest + ".part");
                    FileOutputStream out = new FileOutputStream(tmp);
                    byte[] buf = new byte[65536];
                    long got = 0;
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        got += n;
                        if (total > 0) progress = (int) (got * 100 / total);
                    }
                    out.close();
                    in.close();
                    new File(dest).delete();
                    if (!tmp.renameTo(new File(dest))) throw new Exception("rename failed");
                    progress = 100;
                    dlState = DL_DONE;
                } catch (Exception e) {
                    error = String.valueOf(e);
                    dlState = DL_FAILED;
                }
            }
        }).start();
    }

    public static int getDlState() { return dlState; }

    public static int getProgress() { return progress; }

    /* ---- install ------------------------------------------------------ */

    /** Hand a downloaded APK to the system package installer. */
    public static void installApk(String path) {
        android.content.Context ctx = SDLActivity.getContext();
        if (ctx == null) return;
        File apk = new File(path);
        if (!apk.exists()) { error = "downloaded file missing: " + path; dlState = DL_FAILED; return; }
        Uri uri = FileProvider.getUriForFile(ctx, ctx.getPackageName() + ".files", apk);
        Intent i = new Intent(Intent.ACTION_INSTALL_PACKAGE);
        i.setData(uri);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(i);
    }

    /* ---- plumbing ----------------------------------------------------- */

    private static HttpURLConnection open(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        // GitHub rejects requests without a UA, and release asset URLs 302 to
        // objects.githubusercontent.com (same protocol, so redirect follows).
        c.setRequestProperty("User-Agent", "RapidEditor-Updater");
        c.setConnectTimeout(10000);
        c.setReadTimeout(20000);
        c.setInstanceFollowRedirects(true);
        return c;
    }

    private static String httpGet(String url) throws Exception {
        HttpURLConnection c = open(url);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        int code = c.getResponseCode();
        if (code != 200) {
            c.disconnect();
            throw new Exception("HTTP " + code + " from " + url);
        }
        BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream(), "UTF-8"));
        StringBuilder sb = new StringBuilder();
        char[] buf = new char[8192];
        int n;
        while ((n = r.read(buf)) > 0) sb.append(buf, 0, n);
        r.close();
        c.disconnect();
        return sb.toString();
    }
}
