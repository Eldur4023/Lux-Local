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
# Android has no system libsqlite3: build the vendored amalgamation (FTS5 on,
# Mail's search uses it) and hand it to Lux's own find_path/find_library by
# pre-setting their cache variables -- vendor/lux's CMakeLists stays untouched.
if(ANDROID)
    add_library(sqlite3_vendored STATIC third_party/sqlite/sqlite3.c)
    target_compile_definitions(sqlite3_vendored PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
    set(SQLITE3_INC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite CACHE PATH "" FORCE)
    set(SQLITE3_LIB sqlite3_vendored CACHE STRING "" FORCE)
endif()

add_subdirectory(vendor/lux)

# Lux's own CMakeLists.txt already finds this, but the resulting imported
# target isn't guaranteed visible back up in this scope -- cheap and
# idempotent to ask for it again here.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── Window shell ──────────────────────────────────────────────────────────
# Desktop: GTK3 + WebKitGTK via the vendored webview.h (hard configure error
# if the dev packages are missing). Android: no native window at all -- the
# Kotlin side hosts a WebView and this project builds only libluxlocal.so
# (src/android.cpp), the platform-independent boot.cpp + JNI glue.
# respack runs on the BUILD machine, so a cross build passes the host one:
#   -DLUX_HOST_RESPACK=/path/to/host/build/respack
if(ANDROID)
    if(NOT LUX_HOST_RESPACK)
        message(FATAL_ERROR "Android build: build respack on the host first and pass -DLUX_HOST_RESPACK=<path>")
    endif()
    add_executable(respack IMPORTED)
    set_target_properties(respack PROPERTIES IMPORTED_LOCATION ${LUX_HOST_RESPACK})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LUX_GTK REQUIRED gtk+-3.0)
    pkg_check_modules(LUX_WEBKIT QUIET webkit2gtk-4.1)
    if(NOT LUX_WEBKIT_FOUND)
        pkg_check_modules(LUX_WEBKIT REQUIRED webkit2gtk-4.0)
    endif()
    # ── respack: embeds an app's source directory into a generated .cpp ───
    add_executable(respack tools/respack/respack.cpp)
    target_include_directories(respack PRIVATE include)
endif()

# Shared by every target that embeds the Lux runtime + native window
# (the packaged `app` binary below and the live `app-dev` one).
function(_lux_desktop_link name)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${LUX_GTK_INCLUDE_DIRS} ${LUX_WEBKIT_INCLUDE_DIRS})
    target_link_directories(${name} PRIVATE
        ${LUX_GTK_LIBRARY_DIRS} ${LUX_WEBKIT_LIBRARY_DIRS})
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
    # ./data/ is runtime state (SQLite...), never embedded: re-extracting a
    # build-time copy over it at boot would destroy the user's data.
    list(FILTER _app_files EXCLUDE REGEX "${APP_DIR}/data/.*")
    set(_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/${name}_resources.cpp)
    add_custom_command(
        OUTPUT  ${_generated}
        COMMAND respack ${APP_DIR} ${_generated}
        DEPENDS respack ${_app_files}
        COMMENT "luxdesktop: embedding resources for ${name}"
        VERBATIM
    )

    if(ANDROID)
        # The Activity loads this as System.loadLibrary("luxlocal"): the name
        # is fixed, whatever the app is called.
        add_library(luxlocal SHARED src/android.cpp src/boot.cpp ${_generated})
        target_include_directories(luxlocal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(luxlocal PRIVATE
            -Wl,--whole-archive lux_script -Wl,--no-whole-archive
            lux Threads::Threads log)
    else()
        add_executable(${name} src/runtime.cpp src/boot.cpp src/desktop_window.cpp ${_generated})
        target_compile_definitions(${name} PRIVATE LUXDESKTOP_APP_ID="${APP_ID}")
        _lux_desktop_link(${name})
    endif()
endfunction()

# ── The app ───────────────────────────────────────────────────────────────
# app/ is where the work happens: app.lux, templates/, static assets, window
# title/size (via the window: block) -- everything. This line is the only
# thing in this file that ever names it.
add_lux_desktop_app(app DIR ${CMAKE_CURRENT_SOURCE_DIR}/app)

if(NOT ANDROID)
# ── Dev mode (src/dev.cpp) ───────────────────────────────────────────────
# Reads app/ straight off disk and hot-reloads it through LuxScript's own
# compiler -- no respack, no rebuilding THIS binary after editing app.lux, a
# template or a static file. Run ./build/app-dev while you work.
add_executable(app-dev src/dev.cpp src/desktop_window.cpp)
target_compile_definitions(app-dev PRIVATE
    LUXDESKTOP_APP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/app")
_lux_desktop_link(app-dev)
endif()
