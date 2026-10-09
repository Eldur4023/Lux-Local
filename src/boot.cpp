#include "boot.hpp"
#include "resources.hpp"

#include <lux/logger.hpp>
#include <lux_script/vm.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

// Binds to loopback with port 0 (the OS picks a free ephemeral port), reads
// it back with getsockname(), then releases it immediately. A small race
// (something else could grab the same port before Lux's own bind) is the
// same trade-off every "find a free port" helper makes; fine for an app
// that only ever talks to itself.
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

LuxServer::LuxServer(const fs::path& work_dir) {
    fs::create_directories(work_dir);
    extract_resources(work_dir);
    fs::current_path(work_dir); // the app's own paths (templates "./templates",
                                 // static "/x" -> "./public") are written
                                 // relative to itself, so this is the CWD
                                 // they expect.

    std::vector<fs::path> inputs;
    std::string error;
    if (!lux_script::resolve_inputs({"."}, inputs, error)) throw std::runtime_error(error);

    lux_script::DiagnosticBag diags;
    mod_ = lux_script::compile(inputs, diags);
    if (!diags.empty()) throw std::runtime_error(lux_script::format_errors(diags, mod_->files));

    auto mod = mod_;
    app_.set_templates(mod->program.app.templates_dir);
    for (const auto& m : mod->program.app.statics)
        app_.serve_static(m.url_prefix, m.fs_root, m.spa);

    auto dispatch = [mod](lux::Request& req, lux::Response& res) -> lux::Task<void> {
        auto match = mod->router.match(req.method, req.path);
        if (!match.found) {
            res.status(404).json_text(R"({"error":"Not Found"})");
            co_return;
        }
        req.params = std::move(match.params);
        co_await match.handler(req, res);
    };
    app_.any("/",  dispatch);
    app_.any("/*", dispatch);

    app_.on_error([mod](int code, lux::Request& req, lux::Response& res) {
        auto it = mod->error_handlers.find(code);
        if (it == mod->error_handlers.end()) it = mod->error_handlers.find(0);
        if (it == mod->error_handlers.end()) return;
        lux_script::NativeCtx ctx{req, res};
        ctx.error_code    = code;
        ctx.error_message = res.status_code() >= 500 ? "internal error" : "invalid request";
        lux_script::VM vm;
        auto result = vm.start(*it->second, {}, ctx, &mod->functions, nullptr);
        if (result.status == lux_script::VM::Status::Done &&
            !ctx.response_written && !result.value.is_null())
            res.header("Content-Type", "application/json; charset=utf-8")
               .send(result.value.to_json_text());
        res.status(code);
    });
}

uint16_t LuxServer::start() {
    uint16_t port = find_free_port();
    thread_ = std::thread([this, port] { app_.run("127.0.0.1", port); });
    if (!wait_for_server(port, std::chrono::milliseconds(5000)))
        lux::log().warn("server did not come up in time, continuing anyway");
    return port;
}

void LuxServer::stop() {
    if (!thread_.joinable()) return;
    std::raise(SIGTERM);
    thread_.join();
}
