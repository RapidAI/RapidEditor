#include "np_webview.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>            // Microsoft::WRL::Callback / ComPtr (Windows SDK)
#include <string>
#include "WebView2.h"

/* mingw-w64's wrl.h ships ComPtr but not Microsoft::WRL::Callback. Provide
 * the two tiny COM completion handlers this file needs on that toolchain;
 * MSVC keeps using the standard WRL helper. */
#if defined(__MINGW32__)
#define RE_WRL_EMBEDDED_CALLBACK 1
#else
using Microsoft::WRL::Callback;
#endif
using Microsoft::WRL::ComPtr;

namespace {

/* Loader functions are exported by WebView2Loader.dll but are intentionally
 * NOT declared in the SDK headers (Microsoft ships them as a stable ABI).
 * We resolve them at runtime so a missing loader/runtime degrades gracefully
 * to the system-browser fallback instead of failing to start the process. */
typedef HRESULT (__stdcall *PFNCreateEnvWithOptions)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler);

HMODULE                g_loader    = nullptr;
PFNCreateEnvWithOptions g_createEnv = nullptr;
ComPtr<ICoreWebView2Environment> g_environment;
ComPtr<ICoreWebView2Controller>  g_controller;
ComPtr<ICoreWebView2>            g_webview;
HWND                   g_parent    = nullptr;
RECT                   g_bounds    = { 0, 0, 0, 0 };
bool                   g_visible   = true;
bool                   g_ready     = false;
void (*g_callback)(int) = nullptr;
std::wstring           g_pending_url;

void notify(int ev) { if (g_callback) g_callback(ev); }

std::wstring utf8_to_wide(const char *s) {
    if (!s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

#ifdef RE_WRL_EMBEDDED_CALLBACK
class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    volatile LONG refs_ = 1;
    HRESULT (*fn_)(HRESULT, ICoreWebView2Environment *);
public:
    explicit EnvHandler(HRESULT (*fn)(HRESULT, ICoreWebView2Environment *)) : fn_(fn) {}
    STDMETHODIMP QueryInterface(REFIID id, void **out) override {
        if (id == IID_IUnknown || id == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
            *out = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
            AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r;
    }
    STDMETHODIMP Invoke(HRESULT hr, ICoreWebView2Environment *env) override { return fn_(hr, env); }
};

class ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    volatile LONG refs_ = 1;
    HRESULT (*fn_)(HRESULT, ICoreWebView2Controller *);
public:
    explicit ControllerHandler(HRESULT (*fn)(HRESULT, ICoreWebView2Controller *)) : fn_(fn) {}
    STDMETHODIMP QueryInterface(REFIID id, void **out) override {
        if (id == IID_IUnknown || id == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
            *out = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
            AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r;
    }
    STDMETHODIMP Invoke(HRESULT hr, ICoreWebView2Controller *c) override { return fn_(hr, c); }
};
#endif

/* Stage 2: controller created -> grab ICoreWebView2, apply pending state,
 * navigate to whatever URL the editor asked for before we were ready. */
HRESULT on_controller_created(HRESULT result, ICoreWebView2Controller *controller) {
    if (FAILED(result) || !controller) {
        notify(1);
        return result;
    }
    g_controller = controller;
    controller->put_Bounds(g_bounds);
    controller->put_IsVisible(g_visible ? TRUE : FALSE);
    if (SUCCEEDED(controller->get_CoreWebView2(&g_webview)) && g_webview) {
        /* Suppress the browser's own context menu so right-click inside the
         * preview does not fight with the editor's interaction model. */
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
        }
        if (!g_pending_url.empty())
            g_webview->Navigate(g_pending_url.c_str());
    }
    g_ready = true;
    notify(0);
    return S_OK;
}

/* Stage 1: environment created -> create the child controller bound to the
 * editor's top-level HWND. */
HRESULT on_environment_created(HRESULT result, ICoreWebView2Environment *environment) {
    if (FAILED(result) || !environment) {
        notify(1);
        return result;
    }
    g_environment = environment;
#ifdef RE_WRL_EMBEDDED_CALLBACK
    HRESULT hr = environment->CreateCoreWebView2Controller(
        g_parent, new ControllerHandler(on_controller_created));
#else
    HRESULT hr = environment->CreateCoreWebView2Controller(
        g_parent,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT r, ICoreWebView2Controller *c) -> HRESULT {
                return on_controller_created(r, c);
            }).Get());
#endif
    if (FAILED(hr)) notify(1);
    return hr;
}

} // namespace

extern "C" {

int np_webview_init(void *hwnd, void (*cb)(int event)) {
    if (g_loader) return 1;                 /* already started */
    g_parent = (HWND)hwnd;
    g_callback = cb;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); /* ignore RPC_E_CHANGED_MODE */

    g_loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!g_loader) { notify(1); return 0; }
    g_createEnv = (PFNCreateEnvWithOptions)GetProcAddress(
        g_loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!g_createEnv) {
        FreeLibrary(g_loader); g_loader = nullptr;
        notify(1);
        return 0;
    }

#ifdef RE_WRL_EMBEDDED_CALLBACK
    HRESULT hr = g_createEnv(nullptr, nullptr, nullptr,
                             new EnvHandler(on_environment_created));
#else
    HRESULT hr = g_createEnv(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT r, ICoreWebView2Environment *e) -> HRESULT {
                return on_environment_created(r, e);
            }).Get());
#endif
    if (FAILED(hr)) {
        FreeLibrary(g_loader); g_loader = nullptr; g_createEnv = nullptr;
        notify(1);
        return 0;
    }
    return 1;
}

bool np_webview_ready(void) { return g_ready; }

void np_webview_set_bounds(int x, int y, int w, int h) {
    g_bounds.left = x; g_bounds.top = y;
    g_bounds.right = x + w; g_bounds.bottom = y + h;
    if (g_controller) g_controller->put_Bounds(g_bounds);
}

void np_webview_set_visible(bool visible) {
    g_visible = visible;
    if (g_controller) g_controller->put_IsVisible(visible ? TRUE : FALSE);
}

void np_webview_navigate(const char *url) {
    std::wstring w = utf8_to_wide(url);
    if (g_webview) {
        g_webview->Navigate(w.c_str());
    } else {
        g_pending_url = w;                  /* navigate once the controller is up */
    }
}

void np_webview_reload(void) {
    if (g_webview) g_webview->Reload();
}

void np_webview_shutdown(void) {
    g_webview.Reset();
    g_controller.Reset();
    g_environment.Reset();
    if (g_loader) { FreeLibrary(g_loader); g_loader = nullptr; }
    g_createEnv = nullptr;
    g_ready = false;
    g_callback = nullptr;
    g_pending_url.clear();
}

} // extern "C"

#endif /* _WIN32 */
