#include "boot.hpp"
#include "resources.hpp"

#include <lux_script/project.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

int lux_main(int argc, char** argv);   // vendor/lux main.cpp, compiled with LUX_EMBEDDED

namespace fs = std::filesystem;

namespace {

void extract_resources(const fs::path& dir) {
    for (const auto& f : kEmbeddedFiles) {
        fs::path dest = dir / f.path;
        fs::create_directories(dest.parent_path());
        std::ofstream out(dest, std::ios::binary);
        out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size));
    }
}

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

bool port_open(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

} // namespace

LuxServer::LuxServer(const fs::path& work_dir) {
    fs::create_directories(work_dir);
    extract_resources(work_dir);
    fs::current_path(work_dir); // the app's own relative paths (./state.db, ./public) expect this CWD

    // The server must only ever be reachable from this phone: Lux binds to whatever the app declares (all
    // interfaces if it declares nothing), so refuse to start unless app.lux says `host "127.0.0.1"`.
    // (Same check as the desktop shell's runtime.cpp.)
    std::vector<fs::path> inputs;
    std::string error;
    if (!lux_script::resolve_inputs({"."}, inputs, error)) throw std::runtime_error(error);
    lux_script::DiagnosticBag diags;
    auto mod = lux_script::compile(inputs, diags);
    if (!diags.empty()) throw std::runtime_error(lux_script::format_errors(diags, mod->files));
    const std::string& host = mod->program.app.host;
    if (host != "127.0.0.1" && host != "localhost" && host != "::1")
        throw std::runtime_error("app.lux must declare `host \"127.0.0.1\"` (found \"" + host + "\"): the UI server is for this machine only");
}

int LuxServer::start() {
    const uint16_t port = find_free_port();
    // lux_main keeps the pointers it is given, so they live as long as the process.
    static std::string port_arg;
    port_arg = std::to_string(port);
    static char arg0[] = "lux", arg1[] = "--no-watch", arg2[] = "--port", arg4[] = ".";
    static char* lux_argv[] = {arg0, arg1, arg2, port_arg.data(), arg4, nullptr};
    exited_ = false;
    thread_ = std::thread([this] { lux_main(5, lux_argv); exited_ = true; });
    // A cold start compiles the whole app and opens SQLite: allow it a while on a phone.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline && !exited_) {
        if (port_open(port)) return port;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

void LuxServer::stop() {
    if (!thread_.joinable()) return;
    if (!exited_) std::raise(SIGTERM);   // already exited (compile error): nobody handles SIGTERM, do not raise it
    thread_.join();
}
