#pragma once
#ifdef LUXDESKTOP_LUXIUM
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

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
    // Usable here and now: binary found, an X11 display, and not disabled by LUX_DESKTOP_BACKEND=webkit.
    static bool available();

    explicit LuxiumWindow(WindowOptions opts);
    ~LuxiumWindow() override;

    void run(const std::string& url) override;
    void terminate() override;

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
    void set_menu(const lux_script::Value& spec) override;      // not supported by the host (yet)
    void set_tray(const std::string& icon_path, const std::string& tooltip) override;   // idem
    std::string clipboard_read() override;
    void        clipboard_write(const std::string& text) override;
    int last_width()  const override { return last_width_.load(); }
    int last_height() const override { return last_height_.load(); }

private:
    bool send(const std::string& json);          // thread-safe, false if disconnected
    int  connect_host();                         // connects, spawning the host if needed
    std::string pick(const std::string& suggested_name, bool save_mode, bool folder);

    WindowOptions     opts_;
    int               fd_ = -1;
    std::mutex        send_mutex_;
    std::atomic<bool> stop_{false};
    std::atomic<int>  last_width_{0}, last_height_{0};
    std::mutex              res_mutex_;          // clipboard_read() answers
    std::condition_variable res_cv_;
    std::map<int, std::string> results_;
    std::atomic<int>  next_id_{1};
    bool              menu_warned_ = false;
};
#endif
