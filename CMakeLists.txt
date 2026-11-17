cmake_minimum_required(VERSION 3.16)

# On Apple, OBJCXX must be declared together with the project() call: enabling
# it later via enable_language() breaks on CMake >= 3.30 runners with
# "required internal CMake variable not set: CMAKE_OBJCXX_COMPILE_OBJECT".
if(APPLE)
    project(RapidEditor
        VERSION 1.2.0
        DESCRIPTION "A fast multi-tab notepad and source-code editor built on CUI"
        HOMEPAGE_URL "https://github.com/RapidAI/RapidEditor"
        LANGUAGES C CXX OBJCXX)
else()
    project(RapidEditor
        VERSION 1.2.0
        DESCRIPTION "A fast multi-tab notepad and source-code editor built on CUI"
        HOMEPAGE_URL "https://github.com/RapidAI/RapidEditor"
        LANGUAGES C CXX)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources contain UTF-8 Chinese string literals. MSVC otherwise assumes the
# active code page (GBK on zh-CN) and mangles them, breaking string literals.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# Vendored UI library (https://github.com/yappologistic/CUI).
# The vendored copy carries local patches: CJK caret advance fixes, large-file
# (100 MB+) editing support, and macOS "Open With" Apple Event support.
# See docs/NOTES.md in this repository.
# ---------------------------------------------------------------------------
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CUI_PLATFORM       "auto" CACHE STRING "CUI window backend (auto/win32/cocoa/x11)")
# PNG decoding is needed to show the LaTeX preview (xelatex -> PDF -> pdftoppm PNG).
set(CUI_STB_IMAGE      ON  CACHE BOOL "" FORCE)

# Android: build SDL2 as a subproject (RE_SDL2_SOURCE points at an SDL2
# source tree) so every ANDROID_ABI gets a matching libSDL2, and CUI's
# sdl2 backend links it without find_package. Desktop builds keep using
# the system SDL2 via CUI's find_package.
if(ANDROID)
    if(EXISTS "${RE_SDL2_SOURCE}")
        add_subdirectory("${RE_SDL2_SOURCE}" sdl2-build EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "Android build needs RE_SDL2_SOURCE pointing at an SDL2 source tree (got '${RE_SDL2_SOURCE}')")
    endif()
endif()

add_subdirectory(vendor/CUI)

# ---------------------------------------------------------------------------
# RapidEditor executable
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

set(RE_SOURCES src/rapideditor.c src/np_preview.c src/np_font_bundle.c
               src/np_updater.c)

# Detect WebKitGTK early on Linux so we pick the right source and link libs.
set(RE_HAVE_WEBKITGTK OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WEBKITGTK QUIET webkit2gtk-4.1)
        if(WEBKITGTK_FOUND)
            set(RE_HAVE_WEBKITGTK ON)
        endif()
    endif()
endif()

# Native embedded WebView backend (one per platform, same np_webview.h API).
if(WIN32)
    # WebView2 C++ COM bridge; dynamically loads WebView2Loader.dll (Evergreen).
    list(APPEND RE_SOURCES src/np_webview2_bridge.cpp)
elseif(APPLE)
    # WKWebView (WebKit.framework, part of macOS).
    list(APPEND RE_SOURCES src/np_webview_cocoa.mm)
elseif(ANDROID)
    # Android pad: no embedded WebView yet; HTML preview opens in the system
    # browser through SDL_OpenURL (ACTION_VIEW intent).
    list(APPEND RE_SOURCES src/np_webview_stub.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(RE_HAVE_WEBKITGTK)
        list(APPEND RE_SOURCES src/np_webview_gtk.cpp)
    else()
        # No WebKitGTK: HTML preview falls back to the system browser.
        list(APPEND RE_SOURCES src/np_webview_stub.cpp)
        message(STATUS "WebKitGTK not found: HTML preview uses system-browser fallback")
    endif()
endif()

if(ANDROID)
    # Android (pad, DeX/PC-mode): the editor builds as a shared library that
    # SDL2's Java activity loads. `main` is renamed to SDL_main, which
    # SDLActivity resolves on a dedicated native thread.
    add_library(RapidEditor SHARED ${RE_SOURCES})
    target_link_libraries(RapidEditor PRIVATE cui::cui Threads::Threads)
    target_compile_definitions(RapidEditor PRIVATE main=SDL_main)
    # liblog: __android_log_print diagnostics from np_android_bootstrap/main.
    target_link_libraries(RapidEditor PRIVATE log)
else()
    add_executable(RapidEditor ${RE_SOURCES})
    target_link_libraries(RapidEditor PRIVATE cui::cui Threads::Threads)

    # Set WIN32_EXECUTABLE / dead-strip / LTO flags and the GUI subsystem entry point.
    cui_app_target(RapidEditor)
endif()

if(WIN32)
    # WebView2 C++ headers (loader functions are resolved via LoadLibrary, so
    # no import .lib is needed).
    target_include_directories(RapidEditor PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/webview2/include")

    # CommandLineToArgvW / LocalFree for UTF-8 command lines; ws2_32 for the
    # built-in loopback HTTP server used by the HTML Web preview.
    target_link_libraries(RapidEditor PRIVATE shell32 ws2_32)

    # Copy the architecture-matching WebView2Loader.dll next to the exe so the
    # runtime loader can find it. Detect the target CPU from the toolchain or
    # the MSVC -A platform, with CMAKE_SIZEOF_VOID_P / PROCESSOR as fallbacks.
    # RE_TARGET_ARCH can be forced by CI (MSVC + Ninja cross-compile has no
    # CMAKE_GENERATOR_PLATFORM and reports the host processor).
    string(TOLOWER "${RE_TARGET_ARCH}" RE_FORCE_ARCH)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" RE_PROC_LOWER)
    if(RE_FORCE_ARCH MATCHES "arm64|aarch64")
        set(RE_WV2_ARCH arm64)
    elseif(RE_FORCE_ARCH MATCHES "x64|amd64|x86_64")
        set(RE_WV2_ARCH x64)
    elseif(RE_PROC_LOWER MATCHES "arm64|aarch64")
        set(RE_WV2_ARCH arm64)
    elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
        set(RE_WV2_ARCH arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(RE_WV2_ARCH x64)
    else()
        set(RE_WV2_ARCH x86)
    endif()
    set(RE_WV2_DLL "${CMAKE_CURRENT_SOURCE_DIR}/vendor/webview2/${RE_WV2_ARCH}/WebView2Loader.dll")
    if(EXISTS "${RE_WV2_DLL}")
        add_custom_command(TARGET RapidEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${RE_WV2_DLL}" "$<TARGET_FILE_DIR:RapidEditor>/WebView2Loader.dll"
            COMMENT "Copying WebView2Loader.dll (${RE_WV2_ARCH}) next to RapidEditor.exe")
        message(STATUS "WebView2 loader: ${RE_WV2_ARCH} (${RE_WV2_DLL})")
    else()
        message(WARNING "WebView2Loader.dll not found at ${RE_WV2_DLL}")
    endif()

    # Embed the app icon + version info as a native Windows resource so the
    # title bar / taskbar show the icon without shipping a separate file.
    set(RE_RC_OUT "${CMAKE_CURRENT_BINARY_DIR}/rapideditor.rc")
    # windres mis-parses include paths containing spaces, and cui::cui would
    # propagate such a path to every source. Compile the resource in its own
    # object library with no linked dependencies, then link it into the app.
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/rapideditor.ico" RE_ICON_ABS)
    configure_file(src/rapideditor.rc.in "${RE_RC_OUT}" @ONLY)
    enable_language(RC)
    add_library(re_res OBJECT "${RE_RC_OUT}")
    set_source_files_properties("${RE_RC_OUT}" PROPERTIES INCLUDE_DIRECTORIES "")
    target_link_libraries(RapidEditor PRIVATE re_res)

elseif(APPLE)
    # OBJCXX language is enabled by the conditional project() call at the top
    # (WKWebView: Objective-C++ source).
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(WEBKIT_LIBRARY WebKit REQUIRED)
    target_link_libraries(RapidEditor PRIVATE ${COCOA_LIBRARY} ${WEBKIT_LIBRARY})

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(RE_HAVE_WEBKITGTK)
        target_include_directories(RapidEditor PRIVATE ${WEBKITGTK_INCLUDE_DIRS})
        target_link_libraries(RapidEditor PRIVATE ${WEBKITGTK_LIBRARIES})
        target_compile_options(RapidEditor PRIVATE ${WEBKITGTK_CFLAGS_OTHER})
        target_link_options(RapidEditor PRIVATE ${WEBKITGTK_LDFLAGS_OTHER})
        message(STATUS "WebKitGTK found: HTML preview uses embedded WebKitGTK")
    endif()
endif()

set_target_properties(RapidEditor PROPERTIES OUTPUT_NAME "RapidEditor")

install(TARGETS RapidEditor RUNTIME DESTINATION bin LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION bin)

if(ANDROID)
    set(RE_BACKEND sdl2)
elseif(WIN32)
    # Ship the matching loader alongside the exe in install/dist payloads.
    install(FILES "${RE_WV2_DLL}" DESTINATION bin)
    set(RE_BACKEND win32)
elseif(APPLE)
    set(RE_BACKEND cocoa)
else()
    set(RE_BACKEND x11)
endif()
message(STATUS "RapidEditor ${PROJECT_VERSION} | backend=${RE_BACKEND} | system=${CMAKE_SYSTEM_NAME} | arch=${CMAKE_SYSTEM_PROCESSOR}")
