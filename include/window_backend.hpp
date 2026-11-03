#pragma once
#include <string>

#include <lux_script/value.hpp>

// What the runtime needs from "a native window around a URL". Two Linux
// implementations: DesktopWindow (GTK3 + WebKitGTK, desktop_window.cpp) and
// LuxiumWindow (a window in the shared Luxium app host, luxium_window.cpp);
// Windows only has DesktopWindow. runtime.cpp picks one at startup.
struct WindowOptions {
    std::string title     = "Lux Desktop";
    int         width     = 1024;
    int         height    = 768;
    bool        resizable = true;
    bool        dark      = false;
    bool        devtools  = false;
    std::string icon;             // path to an image file, or empty for none
    std::string app_id;           // stable id of the app (Luxium keeps one profile per id)
    std::string engine = "auto"; // Luxium backend: "luxium" own engine, "web" Chromium, "auto" own engine unless the page runs JavaScript
    std::string wm_class;         // X11 WM_CLASS of the Luxium window (match the .desktop file's StartupWMClass); "" = app_id
};

// All methods are thread-safe except run(), which must be called on the
// thread that constructed the object (see DesktopWindow for the rationale).
class WindowBackend {
public:
    virtual ~WindowBackend() = default;

    // Shows `url` and blocks until the window is closed.
    virtual void run(const std::string& url) = 0;
    // Makes run() return, as if the user had closed the window.
    virtual void terminate() = 0;

    // Reloads the page (dev mode: the sources changed on disk).
    virtual void reload() = 0;

    virtual void set_title(const std::string& title) = 0;
    virtual void minimize() = 0;
    virtual void maximize() = 0;
    virtual void restore() = 0;
    virtual void fullscreen() = 0;
    virtual void unfullscreen() = 0;
    virtual void set_always_on_top(bool on_top) = 0;

    // Blocking native dialogs; "" if the user canceled.
    virtual std::string pick_file(const std::string& suggested_name, bool save_mode) = 0;
    virtual std::string pick_folder() = 0;

    virtual void notify(const std::string& title, const std::string& body) = 0;
    virtual void set_menu(const lux_script::Value& spec) = 0;
    virtual void set_tray(const std::string& icon_path, const std::string& tooltip) = 0;
    virtual std::string clipboard_read() = 0;
    virtual void        clipboard_write(const std::string& text) = 0;

    // Window size as of its last resize (valid after run() returns).
    virtual int last_width() const = 0;
    virtual int last_height() const = 0;
};
