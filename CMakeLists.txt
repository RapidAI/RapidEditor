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
add_subdirectory(vendor/CUI)

# ---------------------------------------------------------------------------
# RapidEditor executable
# ---------------------------------------------------------------------------
add_executable(RapidEditor src/rapideditor.c)
target_link_libraries(RapidEditor PRIVATE cui::cui)

# Set WIN32_EXECUTABLE / dead-strip / LTO flags and the GUI subsystem entry point.
cui_app_target(RapidEditor)

if(WIN32)
    # CommandLineToArgvW / LocalFree are used in main() for UTF-8 command lines.
    target_link_libraries(RapidEditor PRIVATE shell32)
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
