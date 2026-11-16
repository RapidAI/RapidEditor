cmake_minimum_required(VERSION 3.16)

project(RapidEditor
    VERSION 1.1.3
    DESCRIPTION "A fast multi-tab notepad and source-code editor built on CUI"
    HOMEPAGE_URL "https://github.com/RapidAI/RapidEditor"
    LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
add_subdirectory(vendor/CUI)

# ---------------------------------------------------------------------------
# RapidEditor executable
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_executable(RapidEditor src/rapideditor.c src/np_preview.c)
target_link_libraries(RapidEditor PRIVATE cui::cui Threads::Threads)

# Set WIN32_EXECUTABLE / dead-strip / LTO flags and the GUI subsystem entry point.
cui_app_target(RapidEditor)

if(WIN32)
    # CommandLineToArgvW / LocalFree for UTF-8 command lines; ws2_32 for the
    # built-in loopback HTTP server used by the HTML Web preview.
    target_link_libraries(RapidEditor PRIVATE shell32 ws2_32)

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
endif()

set_target_properties(RapidEditor PROPERTIES OUTPUT_NAME "RapidEditor")

install(TARGETS RapidEditor RUNTIME DESTINATION bin)

if(WIN32)
    set(RE_BACKEND win32)
elseif(APPLE)
    set(RE_BACKEND cocoa)
else()
    set(RE_BACKEND x11)
endif()
message(STATUS "RapidEditor ${PROJECT_VERSION} | backend=${RE_BACKEND} | system=${CMAKE_SYSTEM_NAME} | arch=${CMAKE_SYSTEM_PROCESSOR}")
