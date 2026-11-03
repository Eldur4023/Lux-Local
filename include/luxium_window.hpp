#pragma once
#ifdef LUXDESKTOP_LUXIUM
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

#include "sni_tray.hpp"
#include "window_backend.hpp"

// A window in the shared Luxium app host (`luxium --host`): every Lux Desktop
// app on the machine talks to ONE Luxium browser process over a unix socket
// ($XDG_RUNTIME_DIR/luxium/host.sock, one JSON object per line) and gets one
// chrome-less window each. Protocol: Luxium/docs/HOST.md. The connection is
// the window's lifeline -- if this process dies, the host closes the window.
class LuxiumWindow final : public WindowBackend {
public:
    // Path of the luxium binary (env LUXIUM_BIN, PATH, ~/.local/opt/luxium/luxium), or "" if none.
    static std::string find_binary();
    static void post_loopback(int port, const std::string& path);   // POST (no body) to the app's own server
    // Usable here and now: binary found, an X11 display, and not disabled by LUX_DESKTOP_BACKEND=webkit.
    static bool available();

    explicit LuxiumWindow(WindowOptions opts);
    ~LuxiumWindow() override;

    void run(const std::string& url) override;
    void terminate() override;

    void reload() override;
    void set_title(const std::string& title) override;
    void minimize() override;
    void maximize() override;
    void restore() override;
    void fullscreen() override;
    void unfullscreen() override;
    void set_always_on_top(bool on_top) override;
    std::string pick_file(const std::string& suggested_name, bool save_mode) override;
    std::string pick_folder() override;
    void notify(const std::string& title, const std::string& body) override;
    void set_menu(const lux_script::Value& spec) override;      // menu bar drawn by the host
    void set_tray(const std::string& icon_path, const std::string& tooltip) override;   // StatusNotifierItem, in this process
    std::string clipboard_read() override;
    void        clipboard_write(const std::string& text) override;
    // run() could not get a window (host unreachable/wedged twice, or it refused): see error(); run() returned at once.
    bool failed() const { return failed_; }
    std::string error() const { return error_; }
    int last_width()  const override { return last_width_.load(); }
    int last_height() const override { return last_height_.load(); }

private:
    bool send(const std::string& json);          // thread-safe, false if disconnected
    int  acquire_host();                         // connects to a verified host, spawning/replacing one if needed
    std::string pick(const std::string& suggested_name, bool save_mode, bool folder);
    bool        failed_ = false;
    std::string error_;
    int port_ = 0;   // the app server's loopback port (menu actions are POSTed to it)

    WindowOptions     opts_;
    int               fd_ = -1;
    std::mutex        send_mutex_;
    std::atomic<bool> stop_{false};
    std::atomic<int>  last_width_{0}, last_height_{0};
    std::mutex              res_mutex_;          // clipboard_read() answers
    std::condition_variable res_cv_;
    std::map<int, std::string> results_;
    std::atomic<int>  next_id_{1};
    std::string       menu_spec_;                // last set_menu() JSON, resent if the host was not connected yet
    std::unique_ptr<SniTray> tray_;
    std::mutex        tray_mutex_;
};
#endif
