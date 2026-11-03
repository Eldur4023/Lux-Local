#pragma once
#ifdef LUXDESKTOP_LUXIUM
#include <functional>
#include <string>

// System tray icon as a StatusNotifierItem (org.kde.StatusNotifierItem + com.canonical.dbusmenu) on the
// session bus: what KDE, Cinnamon (xapp), GNOME (AppIndicator extension) and XFCE show. It lives in the app
// process, needs no GTK window or main loop of the caller (it runs its own thread with a private GLib
// context), and re-registers if the desktop's StatusNotifierWatcher restarts.
class SniTray {
public:
    // on_toggle: left click / Activate. on_quit: the menu's "Quit".
    SniTray(std::string id, std::string title, std::function<void()> on_toggle, std::function<void()> on_quit);
    ~SniTray();
    SniTray(const SniTray&) = delete;
    SniTray& operator=(const SniTray&) = delete;

    // Thread-safe; can be called again to change the icon and tooltip.
    void set(const std::string& icon_path, const std::string& tooltip);

private:
    struct Impl;
    Impl* p_;
};
#endif
