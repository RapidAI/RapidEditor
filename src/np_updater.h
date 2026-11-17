/* In-app update against GitHub Releases (Android only; other platforms
 * provide inert stubs so the editor compiles unchanged).
 *
 * Flow: the Java helper class Updater does the network and installer work
 * (TLS and PackageInstaller are Android framework services). The C side
 * starts an operation, then polls state each frame from the update dialog.
 */
#ifndef NP_UPDATER_H
#define NP_UPDATER_H

/* check states */
enum {
    NP_UPD_IDLE = 0,     /* nothing started */
    NP_UPD_RUNNING = 1,  /* HTTP request in flight */
    NP_UPD_OK = 2,       /* release JSON parsed: see latest_tag / apk_url */
    NP_UPD_FAILED = 3,   /* see check_error */
};

/* download states */
enum {
    NP_UPD_DL_IDLE = 0,
    NP_UPD_DL_RUNNING = 1,
    NP_UPD_DL_DONE = 2,    /* APK written to app storage */
    NP_UPD_DL_FAILED = 3,  /* see dl_error */
};

/* Fire the background "latest release" request (safe to call once per dialog
 * open; repeated calls while RUNNING are ignored). */
void np_updater_start_check(void);

int         np_updater_check_state(void);
const char *np_updater_latest_tag(void);    /* "" until OK; e.g. "v1.3.0" */
const char *np_updater_apk_url(void);       /* "" when the release ships no APK asset */
const char *np_updater_check_error(void);   /* "" unless FAILED */

/* Download the APK asset chosen from the release JSON into app storage. */
void        np_updater_start_download(void);
int         np_updater_dl_state(void);
int         np_updater_dl_progress(void);   /* 0..100 */
const char *np_updater_dl_error(void);

/* Hand the downloaded APK to the system installer (requests the
 * "install unknown apps" permission flow on first use). */
void np_updater_install(void);

#endif /* NP_UPDATER_H */
