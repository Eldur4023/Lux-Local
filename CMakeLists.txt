cmake_minimum_required(VERSION 3.20)
project(luxdesktop CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── Lux, vendored as plain source (vendor/lux, copied in as-is, not a git
# submodule: no network dependency to build this project once cloned) ───────
# Built exactly as Lux itself builds it -- nothing here patches its
# CMakeLists.txt or its source. LUX_EXAMPLES/LUX_FUZZ are switched off only
# because this project has no use for Lux's own examples or fuzz harnesses,
# not because anything about them is incompatible.
set(LUX_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LUX_FUZZ     OFF CACHE BOOL "" FORCE)
# Android has no system libsqlite3: build the vendored amalgamation and hand it to Lux's own
# find_path/find_library by pre-setting their cache variables -- vendor/lux's CMakeLists stays untouched.
if(ANDROID)
    add_library(sqlite3_vendored STATIC third_party/sqlite/sqlite3.c)
    target_compile_definitions(sqlite3_vendored PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
    set(SQLITE3_INC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite CACHE PATH "" FORCE)
    set(SQLITE3_LIB sqlite3_vendored CACHE STRING "" FORCE)
endif()

add_subdirectory(vendor/lux)

# Android's NDK libc++ has no C++20 time zone database (lux_script/tz.hpp): Howard Hinnant's date/tz library
# stands in for the `time` module. An app that needs zones ships the IANA text files (set_install, see src/android.cpp).
if(ANDROID)
    add_library(date_tz STATIC third_party/date/src/tz.cpp)
    target_include_directories(date_tz PUBLIC third_party/date/include)
    target_compile_definitions(date_tz PUBLIC USE_OS_TZDB=0 HAS_REMOTE_API=0 AUTO_DOWNLOAD=0)
    target_compile_definitions(date_tz PRIVATE INSTALL=.)
    target_link_libraries(lux_script PRIVATE date_tz)
endif()

# Lux's own CMakeLists.txt already finds this, but the resulting imported
# target isn't guaranteed visible back up in this scope -- cheap and
# idempotent to ask for it again here.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── Window shell ──────────────────────────────────────────────────────────
# Desktop: GTK3 + WebKitGTK via the vendored webview.h. Android: no native window at all -- the Kotlin side
# hosts a WebView and this project builds only libluxlocal.so (src/android.cpp + boot.cpp). respack runs
# on the BUILD machine, so a cross build passes the host one: -DLUX_HOST_RESPACK=/path/to/respack
if(ANDROID)
    if(NOT LUX_HOST_RESPACK)
        message(FATAL_ERROR "Android build: build respack on the host first and pass -DLUX_HOST_RESPACK=<path>")
    endif()
    add_executable(respack IMPORTED)
    set_target_properties(respack PROPERTIES IMPORTED_LOCATION ${LUX_HOST_RESPACK})
elseif(WIN32)
    # Windows: webview.h's WEBVIEW_EDGE backend (WebView2, needs the Evergreen runtime on the user's machine --
    # preinstalled on Windows 11 and updated Windows 10). Its built-in WebView2Loader implementation means only
    # the WebView2 *header* is needed: vendor it from the NuGet package Microsoft.Web.WebView2
    # (build/native/include/WebView2.h; unzip the .nupkg) into third_party/webview2/, or pass
    # -DLUX_WEBVIEW2_INCLUDE_DIR=<dir containing WebView2.h>. No WebView2Loader.dll / .lib is needed.
    find_path(LUX_WEBVIEW2_INCLUDE_DIR WebView2.h
        PATHS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2
              ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/include
        NO_DEFAULT_PATH)
    if(NOT LUX_WEBVIEW2_INCLUDE_DIR)
        message(FATAL_ERROR "WebView2.h not found. Download the NuGet package Microsoft.Web.WebView2 "
            "(https://www.nuget.org/packages/Microsoft.Web.WebView2, a .nupkg is a zip), copy "
            "build/native/include/WebView2.h to third_party/webview2/WebView2.h, or pass "
            "-DLUX_WEBVIEW2_INCLUDE_DIR=<dir containing WebView2.h>.")
    endif()
    # Default: GUI subsystem (no console window). -DLUX_WIN_CONSOLE=ON keeps a console (app-dev / debugging).
    option(LUX_WIN_CONSOLE "Windows: build the app with a console (stdout/stderr visible)" OFF)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LUX_GTK REQUIRED gtk+-3.0)
    pkg_check_modules(LUX_WEBKIT QUIET webkit2gtk-4.1)
    if(NOT LUX_WEBKIT_FOUND)
        pkg_check_modules(LUX_WEBKIT REQUIRED webkit2gtk-4.0)
    endif()
endif()
if(NOT ANDROID)
    # ── respack: embeds an app's source directory into a generated .cpp ───
    add_executable(respack tools/respack/respack.cpp)
    target_include_directories(respack PRIVATE include)
endif()

# Shared by every target that embeds the Lux runtime + native window
# (the packaged `app` binary below).
function(_lux_desktop_link name)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${LUX_GTK_INCLUDE_DIRS} ${LUX_WEBKIT_INCLUDE_DIRS})
    if(WIN32)
        target_include_directories(${name} PRIVATE ${LUX_WEBVIEW2_INCLUDE_DIR})
        # shell32/shlwapi/ole32/oleaut32/version/advapi32: webview.h (WebView2 host); user32/gdi32: window +
        # icons; comdlg32: file dialogs; comctl32: window subclass; dwmapi: dark title bar; ws2_32: sockets.
        set(_lux_win_libs ole32 oleaut32 shlwapi shell32 version advapi32 user32 gdi32 comdlg32 comctl32 dwmapi ws2_32)
        target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        if(LUX_WIN_CONSOLE)
            target_compile_definitions(${name} PRIVATE LUXDESKTOP_CONSOLE)
        else()
            set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE ON)
        endif()
        if(MSVC)
            # MSVC link.exe has no --whole-archive: /WHOLEARCHIVE:lux_script keeps every module's
            # LUX_REGISTER_MODULE object (needs CMake >= 3.24 for the $<LINK_LIBRARY:WHOLE_ARCHIVE,...> form).
            target_link_libraries(${name} PRIVATE
                "$<LINK_LIBRARY:WHOLE_ARCHIVE,lux_script>" lux Threads::Threads ${_lux_win_libs})
            return()
        endif()
        # MinGW / clang-mingw (GNU ld style): the same --whole-archive trick as Linux, below.
        target_link_options(${name} PRIVATE -static-libgcc -static-libstdc++)  # no MinGW runtime DLLs to ship
        target_link_libraries(${name} PRIVATE ${_lux_win_libs})
    else()
    target_link_directories(${name} PRIVATE
        ${LUX_GTK_LIBRARY_DIRS} ${LUX_WEBKIT_LIBRARY_DIRS})
    endif()
    # --whole-archive: liblux_script.a is linked member-by-member, and a
    # module .cpp with nothing else in this binary referencing it (true of
    # sqlite/hash/regex/... whenever the app doesn't happen to call
    # window_config() into pulling it in some other way) would otherwise be
    # silently dropped -- along with the LUX_REGISTER_MODULE side effect
    # that makes `import <it>` work at all. Same trick Lux's own
    # lux_link_script() uses for lux-bin (vendor/lux/CMakeLists.txt), just
    # not reachable from here since that function's scope does not extend
    # back up past add_subdirectory().
    target_link_libraries(${name} PRIVATE
        -Wl,--whole-archive lux_script -Wl,--no-whole-archive
        lux Threads::Threads
        ${LUX_GTK_LIBRARIES} ${LUX_WEBKIT_LIBRARIES})
endfunction()

# Declares one app: DIR's own directory (LuxScript + templates + static
# assets -- window title/size included, see app/app.lux's `window:` block)
# gets embedded into a single native executable called NAME. Nothing about
# the app is read at configure time beyond its file list -- respack runs at
# BUILD time, so editing app.lux and rebuilding is enough, no need to
# reconfigure, and nothing here needs editing when the app's own config
# changes.
function(add_lux_desktop_app name)
    cmake_parse_arguments(APP "" "DIR;ID" "" ${ARGN})
    if(NOT APP_DIR)
        message(FATAL_ERROR "add_lux_desktop_app(${name}): DIR is required")
    endif()
    if(NOT APP_ID)
        set(APP_ID ${name})
    endif()

    file(GLOB_RECURSE _app_files CONFIGURE_DEPENDS ${APP_DIR}/*)
    # ./data/ is the app's own RUNTIME state (SQLite database, WAL files):
    # never embedded into the binary -- extracting a stale build-time copy
    # over it at boot would destroy the user's library.
    list(FILTER _app_files EXCLUDE REGEX "${APP_DIR}/data/.*")
    set(_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/${name}_resources.cpp)
    add_custom_command(
        OUTPUT  ${_generated}
        COMMAND respack ${APP_DIR} ${_generated}
        DEPENDS respack ${_app_files}
        COMMENT "luxdesktop: embedding resources for ${name}"
        VERBATIM
    )

    set(_lux_main ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lux/src/lux_script/main.cpp)
    set_source_files_properties(${_lux_main} PROPERTIES COMPILE_DEFINITIONS LUX_EMBEDDED)
    if(ANDROID)
        # The Activity loads this as System.loadLibrary("luxlocal"): the name is fixed, whatever the app is called.
        add_library(luxlocal SHARED src/android.cpp src/boot.cpp ${_lux_main} ${_generated})
        target_include_directories(luxlocal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(luxlocal PRIVATE
            -Wl,--whole-archive lux_script -Wl,--no-whole-archive
            lux date_tz Threads::Threads log)
        # 16 KB pages (Android 15+ devices; NDK r26 still defaults to 4 KB), and no symbol tables.
        target_link_options(luxlocal PRIVATE -Wl,-z,max-page-size=16384 -Wl,--strip-all)
    else()
        add_executable(${name} src/runtime.cpp src/desktop_window.cpp ${_lux_main} ${_generated})
        target_compile_definitions(${name} PRIVATE LUXDESKTOP_APP_ID="${APP_ID}")
        _lux_desktop_link(${name})
    endif()
endfunction()

# ── The app ───────────────────────────────────────────────────────────────
# app/ is where the work happens: app.lux, templates/, static assets, window
# title/size (via the window: block) -- everything. This line is the only
# thing in this file that ever names it.
# The app: app/ is where the work happens (app.lux, templates, static assets, the window: block). On
# Android the very same directory is embedded into libluxlocal.so.
add_lux_desktop_app(app DIR ${CMAKE_CURRENT_SOURCE_DIR}/app ID app)
