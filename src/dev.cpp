// Dev mode: serves app/ straight off disk -- no respack, no embedding, no rebuilding this binary after
// editing a .lux file, a template or a static asset. It is src/runtime.cpp with two differences: Lux's own
// file watcher stays on (it recompiles .lux files and swaps the live module, see watch_loop in
// vendor/lux's main.cpp), and a second watcher here reloads the window's page whenever anything under app/
// changes, the way a browser tab refreshes. Works with either window backend (Luxium host or WebKitGTK).
#include "desktop_window.hpp"
#include "luxium_window.hpp"
#include "window_hooks.hpp"

#include <lux_script/project.hpp>
#include <lux_script/window_config.hpp>
#include <lux/logger.hpp>

#include <arpa/inet.h>
#include <cctype>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifndef LUXDESKTOP_APP_DIR
#error "LUXDESKTOP_APP_DIR must be set at compile time (see CMakeLists.txt)"
#endif

int lux_main(int argc, char** argv);   // vendor/lux main.cpp, built with LUX_EMBEDDED

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop{false};

// Latest mtime of anything under app/ except data/ (the app's own database changes constantly).
// std::optional, not a default-constructed file_time_type{} as the "nothing seen yet" sentinel: libstdc++'s
// file_clock epoch is not 1970, so a real file's time_since_epoch() can compare as LESS than a default one.
using MTime = std::optional<fs::file_time_type>;

MTime latest_mtime(const fs::path& root) {
    MTime latest;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && it->path().filename() == "data") { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        auto t = fs::last_write_time(it->path(), ec);
        if (!ec && (!latest || t > *latest)) latest = t;
    }
    return latest;
}

void reload_window() {
    std::lock_guard<std::mutex> lk(g_window_mutex);
    if (g_window) g_window->reload();
}

void watch_loop(fs::path root) {
    MTime last = latest_mtime(root);
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const MTime now = latest_mtime(root);
        if (now == last) continue;
        last = now;
        // Lux's watcher polls too: give it time to recompile before the page asks for the new version.
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        reload_window();
    }
}

uint16_t find_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("bind: " + std::string(std::strerror(errno)));
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// Same window-geometry memory as src/runtime.cpp -- see its comments for
// the reasoning (sanity bounds, XDG_CACHE_HOME, why a plain "W H" file).
std::string resolve_display_name(const std::shared_ptr<lux_script::Module>& mod) {
    const auto& wcfg = lux_script::window_config();
    if (!wcfg.title.empty())            return wcfg.title;
    if (!mod->program.app.name.empty()) return mod->program.app.name;
    return "Lux Desktop App";
}

std::string sanitize_id(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "lux-desktop-app" : out;
}

fs::path xdg_cache_home() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return fs::path(xdg);
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".cache";
}

fs::path geometry_file(const std::string& id) {
    return xdg_cache_home() / "lux-desktop" / (id + ".geometry");
}

bool load_saved_geometry(const std::string& id, int& width, int& height) {
    std::ifstream in(geometry_file(id));
    int w = 0, h = 0;
    if (!(in >> w >> h)) return false;
    if (w < 100 || h < 100 || w > 10000 || h > 10000) return false;
    width = w;
    height = h;
    return true;
}

void save_geometry(const std::string& id, int width, int height) {
    if (width < 100 || height < 100 || width > 10000 || height > 10000) return;
    std::error_code ec;
    fs::path f = geometry_file(id);
    fs::create_directories(f.parent_path(), ec);
    std::ofstream out(f);
    if (out) out << width << " " << height << "\n";
}

bool wait_for_server(uint16_t port, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            ::close(fd);
            if (ok) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

int main(int, char** argv) {
    fs::current_path(LUXDESKTOP_APP_DIR); // app.lux's own paths ("./templates", "./public") are relative to itself

    std::vector<fs::path> inputs;
    std::string error;
    if (!lux_script::resolve_inputs({"."}, inputs, error)) {
        std::cerr << "luxdesktop-dev: " << error << "\n";
        return 1;
    }
    lux_script::DiagnosticBag diags;
    auto mod = lux_script::compile(inputs, diags);   // fills window_config()
    if (!diags.empty()) {
        std::cerr << lux_script::format_errors(diags, mod->files);
        return 1;
    }

    uint16_t port = find_free_port();
    std::string port_arg = std::to_string(port);
    static char arg0[] = "lux", arg1[] = "--port", arg3[] = ".";
    char* lux_argv[] = {arg0, arg1, port_arg.data(), arg3, nullptr};
    ::setenv("LUX_THREADS", "2", 0);
    std::thread server_thread([&lux_argv] {
        lux_main(4, lux_argv);   // watch mode stays on: Lux recompiles .lux files itself
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) {
            g_shutdown_from_signal.store(true);
            g_window->terminate();
        }
    });
    if (!wait_for_server(port, std::chrono::milliseconds(20000)))
        lux::log().warn("server did not come up in time, opening the window anyway");

    std::thread watcher(watch_loop, fs::path(LUXDESKTOP_APP_DIR));

    const lux_script::WindowConfig& wcfg = lux_script::window_config();
    std::string app_id = sanitize_id(resolve_display_name(mod));

    WindowOptions opts;
    opts.app_id    = app_id + "-dev";   // own Luxium profile: never mixes with the installed app's
    opts.title     = resolve_display_name(mod);
    opts.width     = wcfg.width;
    opts.height    = wcfg.height;
    opts.resizable = wcfg.resizable;
    opts.devtools  = true; // dev mode: always on, no reason to hide it here
    opts.dark      = wcfg.dark;
    opts.icon      = wcfg.icon;
    {
        const std::string a0 = argv[0] ? argv[0] : "";
        opts.wm_class = a0.substr(a0.find_last_of('/') == std::string::npos ? 0 : a0.find_last_of('/') + 1);
    }
    load_saved_geometry(app_id, opts.width, opts.height);

    std::unique_ptr<WindowBackend> window;
#ifdef LUXDESKTOP_LUXIUM
    if (LuxiumWindow::available()) window = std::make_unique<LuxiumWindow>(opts);
#endif
    if (!window) window = std::make_unique<DesktopWindow>(opts);
    {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        g_window = window.get();
    }
    install_window_control_hooks();
    lux::log().info("lux desktop (dev): watching " + fs::path(LUXDESKTOP_APP_DIR).string());
    window->run("http://127.0.0.1:" + std::to_string(port) + "/");
    save_geometry(app_id, window->last_width(), window->last_height());
    {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        g_window = nullptr;
    }
    window.reset();

    if (!g_shutdown_from_signal.load()) std::raise(SIGTERM);
    // See LuxServer::stop() in boot.cpp: bound the wait for the orderly shutdown.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(4));
        std::_Exit(0);
    }).detach();
    server_thread.join();

    g_stop.store(true);
    watcher.join();
    return 0;
}
