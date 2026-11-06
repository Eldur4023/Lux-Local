#include "boot.hpp"
#include "extract.hpp"

#include <lux_script/native_build.hpp>
#include <lux_script/project.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <netinet/in.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

int lux_main(int argc, char** argv);   // Lux main.cpp, compiled with LUX_EMBEDDED
extern std::function<std::string(const lux_script::AppDecl&)> lux_embedded_vet;   // ditto: its hooks
extern std::function<void()>                                  lux_embedded_listening;

namespace fs = std::filesystem;

namespace {

// Binds to loopback with port 0 (the OS picks a free ephemeral port), reads it back, releases it.
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

} // namespace

LuxServer::LuxServer(const fs::path& work_dir) {
    fs::create_directories(work_dir);
    extract_resources(work_dir);
    fs::current_path(work_dir); // the app's own relative paths (./state.db, ./public) expect this CWD
}

int LuxServer::start() {
    const uint16_t port = find_free_port();
    // lux_main keeps the pointers it is given, so they live as long as the process.
    static std::string port_arg;
    port_arg = std::to_string(port);
    static char arg0[] = "lux", arg1[] = "--no-watch", arg2[] = "--port", arg4[] = ".", arg5[] = "--native";
    static char* lux_argv[] = {arg0, arg1, arg2, port_arg.data(), arg4, arg5, nullptr};
    // The app's native code, when the APK was built with LUX_ANDROID_NATIVE (CMakeLists.txt): there is no
    // compiler on the phone, so --native only makes sense if that code is already inside this library.
    const int argc = lux_script::native_prebuilt_available() ? 6 : 5;
    exited_ = false;
    listening_ = false;
    // The server must only ever be reachable from this phone: Lux binds to whatever the app declares (all
    // interfaces if it declares nothing), so refuse to start unless app.lux says `host "127.0.0.1"`.
    // (Same check as the desktop shell's runtime.cpp.) lux_main runs it on the app it compiles anyway.
    lux_embedded_vet = [](const lux_script::AppDecl& app) -> std::string {
        if (app.host == "127.0.0.1" || app.host == "localhost" || app.host == "::1") return {};
        return "app.lux must declare `host \"127.0.0.1\"` (found \"" + app.host + "\"): the UI server is for this machine only";
    };
    lux_embedded_listening = [this] {
        { std::lock_guard<std::mutex> lk(m_); listening_ = true; }
        cv_.notify_all();
    };
    ::setenv("LUX_THREADS", "2", 0);   // a phone needs no loop per core (override: LUX_THREADS)
    thread_ = std::thread([this, argc] {
        try { lux_main(argc, lux_argv); } catch (const std::exception& e) { std::fprintf(stderr, "lux: %s\n", e.what()); }
        { std::lock_guard<std::mutex> lk(m_); exited_ = true; }
        cv_.notify_all();
    });
    // A cold start compiles the whole app and opens SQLite: allow it a while on a phone. Sleeps until lux_main
    // says it is listening (or ended), no polling.
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::seconds(30), [this] { return listening_ || exited_; });
    return listening_ ? port : -1;
}

void LuxServer::stop() {
    if (!thread_.joinable()) return;
    if (!exited_) std::raise(SIGTERM);   // already exited (compile error): nobody handles SIGTERM, do not raise it
    thread_.join();
}
