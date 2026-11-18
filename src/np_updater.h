/* In-app update against GitHub Releases.
 *
 * Android: the Java helper Updater does the HTTP and PackageInstaller work.
 * Desktop: np_net.c fetches the latest-release JSON and downloads the asset
 * matching this OS/CPU; the downloaded installer is handed to the shell.
 *
 * In both cases the C side starts an operation and the UI polls state each
 * frame from the update dialog.
 */
#ifndef NP_UPDATER_H
#define NP_UPDATER_H

/* check states */
enum {
    NP_UPD_IDLE = 0,     /* nothing started */
    NP_UPD_RUNNING = 1,  /* HTTP request in flight */
    NP_UPD_OK = 2,       /* release JSON parsed: see latest_tag / asset_url */
    NP_UPD_FAILED = 3,   /* see check_error */
};

/* download states */
enum {
    NP_UPD_DL_IDLE = 0,
    NP_UPD_DL_RUNNING = 1,
    NP_UPD_DL_DONE = 2,    /* installer written to disk */
    NP_UPD_DL_FAILED = 3,  /* see dl_error */
};

/* Fire the background "latest release" request (repeated calls while
 * RUNNING are ignored). */
void np_updater_start_check(void);

int         np_updater_check_state(void);
const char *np_updater_latest_tag(void);    /* "" until OK; e.g. "v1.3.0" */

/* The release asset picked for this platform, and the local file it will be
 * / has been downloaded to. Both are "" until the check succeeds. */
const char *np_updater_asset_url(void);
const char *np_updater_asset_name(void);
const char *np_updater_dest_path(void);
const char *np_updater_check_error(void);   /* "" unless FAILED */

/* Kept for the Android dialog; on desktop it returns the same URL as
 * np_updater_asset_url(). */
const char *np_updater_apk_url(void);

/* Download the selected asset. */
void        np_updater_start_download(void);
int         np_updater_dl_state(void);
int         np_updater_dl_progress(void);   /* 0..100 */
const char *np_updater_dl_error(void);

/* Install: hands the APK to Android's installer, or launches the downloaded
 * desktop installer. np_updater_open_folder() (desktop only) reveals it. */
void np_updater_install(void);
void np_updater_open_folder(void);

#endif /* NP_UPDATER_H */
