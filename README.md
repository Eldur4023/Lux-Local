# Lux Desktop

An Electron/Tauri equivalent built on [Lux](vendor/lux) — a native GTK3 +
WebKitGTK window instead of a bundled Chromium, LuxScript instead of Node,
and a build that produces **one native binary**: compile it, run it, the
window opens. Nothing to install alongside it, no runtime to ship
separately, no crate tree to download.

Lux is vendored as plain source under `vendor/lux/` (copied in, not a git
submodule — no network access needed to build this project once it's
checked out), taken from the `dev` branch of the main Lux repo, with one
documented addition that is NOT in Lux: a `window` module
(`vendor/lux/src/lux_script/modules/window.cpp`, `window_config.hpp`,
`window_control.hpp`) using Lux's own drop-in module mechanism, so window
title/size/theme can be declared from LuxScript itself and a route can open
file/folder pickers, notifications, the clipboard... This repo is the home of
that module; apps vendor it from here. Any change to Lux itself is made in the
main Lux repo (`dev`) and then re-vendored, never edited in `vendor/lux/`.
Exception, local patches pending upstreaming: `LUX_THREADS` env var (`src/app.cpp`) and the close-idle-keep-alive-on-drain
shutdown (`tcp_server.*`, `http_connection.*`, ported from Lux commit 706adc1); plus a `gtk_main_quit` guard in
`third_party/webview/webview.h`.
The desktop shell starts the app with Lux's own `main()` (`lux_main()`, built
with `LUX_EMBEDDED`) on a thread, so it behaves exactly like `lux ./app`.

## Where you work

**`app/`** — that's it. `app/app.lux`, `app/templates/`, `app/public/`, and
any other `.lux` file you want; Lux compiles a whole directory tree, order
does not matter. `CMakeLists.txt` never needs to change for anything that
happens inside `app/`.

## How it works

1. **Write the app** in `app/` — LuxScript, the same language and template
   engine as any other Lux project. Window title/size go in `app.lux`'s own
   `window:` block (see below), not in CMake.
2. **Build**: `cmake --build build`. At build time, `respack`
   (`tools/respack/`) embeds `app/`'s whole contents into the binary as byte
   arrays — no external files, no assets folder to keep next to it.
3. **Run `./build/app`.** At startup it extracts its own embedded files into
   a throwaway temp directory, compiles them through Lux's own compiler
   (`lux_script::compile`, the exact same one `lux` itself uses), starts an
   HTTP server bound to `127.0.0.1` on a random free port — nothing outside
   the machine can ever reach it — and opens a native window pointed at it.
   The page just does `fetch()` against the app's own routes; that loopback
   HTTP call *is* the whole "IPC bridge", nothing extra to learn. Closing
   the window (or `CTRL+C`) shuts the server down and deletes the temp
   directory.

## Linux: windows in a shared Luxium

On Linux the window is, by default, a window of the **Luxium app host**
(`luxium --host`, [Luxium](https://github.com/Eldur4023/Luxium)): every Lux
Desktop app on the machine shares ONE Chromium process, each app gets its own
chrome-less window and its own private profile (localStorage/cookies are keyed
by the app id). `src/luxium_window.cpp` speaks the host's line-JSON protocol
(`Luxium/docs/HOST.md`) over `$XDG_RUNTIME_DIR/luxium/host.sock`, starting the host
if it is not running. Luxium is found through `LUXIUM_BIN`, then `PATH`, then
`~/.local/opt/luxium/luxium` (`Luxium/tools/install.sh`). If it is not found, or
`LUX_DESKTOP_BACKEND=webkit` is set, the GTK + WebKitGTK window is used as before;
build without the Luxium backend with `-DLUX_DESKTOP_LUXIUM=OFF`. Windows and Android are untouched.

On the Luxium backend `window.set_menu()` draws a menu bar inside the Luxium window (SDL has no native
menus; same spec, accelerators checked before the page sees the key, same `fetch(action)` delivery, F10 and
arrows to navigate; no mnemonics). `window.set_tray()` is a StatusNotifierItem
(`org.kde.StatusNotifierItem` + `com.canonical.dbusmenu`) published by the app process itself: left click toggles
the window, its menu has Show / Hide and Quit. `dark true` sets the dark title bar hint and the menu colours. File
pickers use `zenity` and notifications `notify-send`; `devtools true` gives F12 (a second window) instead of
WebKit's inspector. The window's WM_CLASS is the program name (as GTK sets it) and `--install-desktop` writes it
as `StartupWMClass`; a hand-written `.desktop` file matches by file name = program name, or add `StartupWMClass=`.

### Which engine draws the window

The Luxium window renders with **Luxium's own engine** (HTML + CSS + LuxScript pages, no JavaScript): no
Chromium process, no `libcef` mapped; roughly 12 MB per extra app window (WebKitGTK: ~120 MB). Chromium is
only started, lazily and only for that window, when the page needs JavaScript. `window: engine` picks:

- `"auto"` (default): own engine; if the loaded page runs JavaScript (a `<script>` that is not LuxScript, or
  `<script src>`) the host hands that window to Chromium. Today's JavaScript-frontend apps (Localify, Mail...)
  keep working unchanged this way.
- `"luxium"`: own engine only, JavaScript is never run.
- `"web"`: Chromium straight away (skips the probe).

Menu actions (`window.set_menu()`) are POSTed to the app's own server by the app process, so they work on every
engine. `LUX_DESKTOP_BACKEND=webkit` still forces WebKitGTK.

## The window: block

```lux
import window

app:
    name      "My App"
    templates "./templates"
    static "/static" -> "./public"

    window:
        title     "My App"
        width     800
        height    600
        resizable true
        devtools  false     # true opens WebKit's inspector
        engine    "auto"    # Luxium backend: "auto" | "luxium" (own engine) | "web" (Chromium)
        icon      "./icon.png"   # path relative to the app's own directory
```

Exactly the same mechanism `sqlite: { file, pool }` already uses for its own
config — `import window` marks it active, and the block's keys get
validated and read by the module itself. If the block (or the whole
`import`) is missing, sensible defaults apply (1024x768, resizable, no
devtools, no icon) and the window falls back to `app: name` for its title.

## Controlling the window from a route

Same `import window`, now called from inside a handler — the window is the
same process, so this is a plain function call, not a message across a
bridge:

```lux
post endpoint("/rename", string name):
    window.set_title(name)
    return { "ok": true }

post endpoint("/minimize"):
    window.minimize()   # also: .maximize(), .restore(), .close(),
                        # .fullscreen(), .unfullscreen(),
                        # .set_always_on_top(true/false)
    return status(204)

get endpoint("/pick-file"):
    string path = await window.open_file()      # "" if the user canceled
    return { "path": path }

get endpoint("/pick-save"):
    string path = await window.save_file("export.csv")
    return { "path": path }

post endpoint("/ping-me"):
    window.notify("Lux Desktop", "Something finished in the background.")
    return status(204)
```

`open_file`/`save_file` open a native GTK file chooser and need `await`:
showing a dialog and waiting on the user is an unbounded wait, so they run
on Lux's shared worker pool instead of the event loop thread — the rest of
the app (and the window itself) stays responsive while the dialog is open.

`notify` sends a real desktop notification (the freedesktop D-Bus spec —
the same thing `notify-send` uses) rather than a browser-style alert. It is
synchronous: a local D-Bus round-trip has no unbounded wait, so it needs no
`await`. No extra dependency either — it talks to
`org.freedesktop.Notifications` over GDBus, which ships with GTK3 already.

## Native menu and system tray

```lux
post endpoint("/setup-chrome"):
    window.set_menu([
        { "label": "File", "items": [
            { "label": "New note", "action": "/notes/new", "accel": "<Control>n" },
            "-",
            { "label": "Quit", "action": "/quit", "accel": "<Control>q" }
        ]},
        { "label": "Help", "items": [
            { "label": "About", "action": "/about" }
        ]}
    ])
    window.set_tray("./icon.png", "My App")
    return status(204)

post endpoint("/quit"):
    window.close()
    return status(204)
```

A menu item's `action` is just a path — clicking it runs
`fetch(action, {method:'POST'})` inside the page, the same "loopback HTTP
is the whole IPC" idea as everything else here, not a second
native-to-LuxScript callback mechanism. `"-"` is a separator. An item can
have `"items"` instead of `"action"` for a nested submenu, as deep as you
like (`File > Export > As CSV`). An optional `"accel"` (GTK accelerator
syntax) binds a window-wide keyboard shortcut to that item, working
whether or not the menu is open. Call `set_menu` again (say, after login)
to replace the whole bar.

A boolean `"checked"` key turns a leaf into a checkbox — GTK renders and
manages its own tick mark, and clicking it calls `action` with the
resulting state appended: `fetch(action + "?checked=true|false", ...)`.
`set_menu` again with the field updated (from wherever your route keeps
that state) to reflect it back if something else changes it.

The tray icon's left-click toggles the window's visibility — built in, not
configurable yet. It uses `GtkStatusIcon`, deprecated since GTK 3.14 but
still the only tray API GTK3 ships without pulling in a separate
`libappindicator` dependency, and it still works with anything that
supports the older XEmbed systray protocol (confirmed on KDE Plasma).

## Clipboard

```lux
post endpoint("/copy", string text):
    window.clipboard_write(text)
    return status(204)

get endpoint("/paste"):
    string text = await window.clipboard_read()
    return { "text": text }
```

The system clipboard (X11 `CLIPBOARD`, via `GtkClipboard`), not the
webview's own `navigator.clipboard` — which needs a user gesture and a
permission prompt WebKit gives no way to pre-approve, so it is not usable
from a script-triggered action. `clipboard_write` is synchronous: claiming
ownership is local and unilateral. `clipboard_read` needs `await` — it
waits on whatever OTHER process currently owns the selection to answer
over X11, which is only as fast as that process feels like being; reading
back something this same app just wrote is instant, reading from a
different, slow-to-respond owner is a real, observed unbounded wait, the
same class of thing `open_file`/`save_file` already needed `await` for.

## Build

Needs `libgtk-3-dev` and `libwebkit2gtk-4.1-dev` (`-4.0-dev` on an older
distribution) — the only things not already vendored. Everything Lux itself
needs (llhttp) is vendored inside `vendor/lux/third_party/`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/app        # the packaged binary -- respack embeds a snapshot of app/
```

## Installing it as a real app

```bash
./build/app --install-desktop
```

Writes a `.desktop` launcher into `~/.local/share/applications/` (and the
window's icon, if it has one, into the standard
`~/.local/share/icons/hicolor/256x256/apps/` theme location) pointing at
this exact binary's own path — the app then shows up in the normal
application menu/launcher like anything installed through a package
manager. Never run automatically: it's the one thing in this whole project
that writes outside its own directory, into the desktop environment
itself, so it only happens when asked for by name.

## Window size is remembered

Resizing the window and reopening the app later restores that size —
saved to `~/.cache/lux-desktop/<app-id>.geometry` (a plain `WIDTH HEIGHT`,
nothing fancier) right as the window closes, and read back before the next
one opens. The `window:` block's `width`/`height` are the first-launch
default, not something the window snaps back to on every run.

## Guarding the local API

Loopback is not private on a shared machine or on a phone: other apps can open the port. An app that wants to protect its API keeps a
secret in `api-token` (mode 600) in its work dir -- created in an `on start:` hook, so it exists before the port opens -- and its routes check it
(Mail: `require api_ok() else status(401)`, header `X-Mail-Token`). The desktop shell (`src/runtime.cpp`) and the Android Activity read that file and
hand the secret to the page in the URL fragment (`#t=...`, never sent to the server); the page puts it on every request. The Android boot also
refuses to start an app that does not declare `host "127.0.0.1"`.

## Dev mode

`./build/app-dev` (built by default on Linux) serves `app/` straight off disk: no respack, no rebuild after
editing. Lux's own watcher recompiles `.lux` files; a second watcher reloads the window when anything under
`app/` (except `data/`) changes. It uses the Luxium backend when available, WebKitGTK otherwise, and has
devtools always on. Its Luxium profile is `<id>-dev`, separate from the installed app's.

## What this is not (yet)

- **Linux only**, same as Lux itself (epoll, `sendfile(2)`, `SO_REUSEPORT`).
- The menu has no radio-button groups yet (nested submenus, keyboard
  accelerators and checkboxes all work).

## Android

The window shell is the only GTK-specific part: `src/boot.cpp` (extract, compile, serve on loopback) is
platform-independent, and `src/android.cpp` exposes it over JNI as `libluxlocal.so` with no GTK/WebKit.
The Kotlin side hosts a `WebView` and loads the port returned by `LuxLocal.start(filesDir)`:

```kotlin
package dev.lux.local
object LuxLocal { init { System.loadLibrary("luxlocal") }
                  external fun start(dataDir: String): Int   // port, -1 on failure
                  external fun stop() }
// webView.loadUrl("http://127.0.0.1:${LuxLocal.start(filesDir.path)}/")
```

Build (NDK r26+, API 28 minimum -- bionic has `posix_spawn` from 28; respack runs on the host):

    cmake -S . -B build && cmake --build build --target respack
    cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
          -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DLUX_HOST_RESPACK=$PWD/build/respack \
          -DLUX_POSTGRES=OFF -DLUX_MYSQL=OFF -DLUX_PDF=OFF -DLUX_HTTP=OFF -DLUX_JEMALLOC=OFF
    cmake --build build-android --target luxlocal

With `http`/`mail`/`imap` (static OpenSSL + libcurl, cross-built by `tools/android-deps.sh`) and SQLite (vendored in
`third_party/sqlite`, FTS5 on; used automatically on Android), and the `time` module through `third_party/date` (Hinnant's
date/tz, because the NDK's libc++ has no C++20 time zone database):

    ANDROID_NDK=$ANDROID_NDK tools/android-deps.sh arm64-v8a      # -> build-android-deps/arm64-v8a
    D=$PWD/build-android-deps/arm64-v8a
    PKG_CONFIG_LIBDIR=$D/lib/pkgconfig cmake -S . -B build-android ... -DLUX_PDF=OFF \
          -DCMAKE_SHARED_LINKER_FLAGS="-L$D/lib"

TLS trusts Android's system CA directory (`/system/etc/security/cacerts`).

Gradle project in `android/` (a plain `Activity` + `WebView` + `LuxLocal.kt`); `tools/android-build.sh` builds the
`.so` for each ABI into `android/app/src/main/jniLibs/`, then:

    ANDROID_NDK=... tools/android-build.sh arm64-v8a x86_64
    cd android && gradle assembleDebug          # JDK 17, Gradle 8.7, SDK 34 (sdk.dir in local.properties)

Checked on an x86_64 emulator (Android 14): the demo app boots, serves over loopback and the +1 button round-trips
to the Lux server. Not tested on a real arm64 device.

The mail modules (`imap`, `mail`, `mailparse`, `keyring`) are part of Lux (`dev`). `keyring` keeps `secret-tool` on desktop and, on
Android, uses `lux_script::keyring_control()` hooks that `src/android.cpp` points at `LuxLocal.secretGet/Set/Delete`
(AES-GCM key in the Android Keystore, ciphertext in private SharedPreferences). `tools/android-deps.sh` pins curl 8.18.0:
8.19.0-8.22.0 return an empty body for custom IMAP commands (`UID FETCH ...`).

`window.notify` is wired to a system notification. Background sync needs no foreground service (so no permanent notification):
`Sync.kt` chains wake-up alarms that call `POST /api/sync` every 2 minutes -- that route is the app's own business (Mail's notifies new mail
through `window.notify`). TLS: `src/android.cpp` builds a PEM bundle from the system CAs (`SSL_CERT_FILE`), which the `http`/`mail`/`imap`
modules hand to curl through `lux_curl_init()`.

The `window` hooks wired on Android: `notify`, `pick_file`/`pick_folder` (system document/folder pickers; downloads go to
Downloads, which needs "All files access": `needs_all_files` in `res/values/config.xml`) and `clipboard_write`. Not wired: tray,
menu, window sizing, `clipboard_read`.
