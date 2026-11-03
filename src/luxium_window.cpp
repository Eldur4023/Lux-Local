#ifdef LUXDESKTOP_LUXIUM
#include "luxium_window.hpp"
#include "applog.hpp"

#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string runtime_dir() {
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return std::string(x) + "/luxium";
    return "/tmp/luxium-" + std::to_string(getuid());
}

std::string jq(const std::string& s) {   // JSON string literal
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

// Minimal reader for the host's flat events: value of "key" as a string/number/bool, or "".
std::string field(const std::string& line, const std::string& key) {
    const std::string pat = "\"" + key + "\":";
    size_t p = line.find(pat);
    if (p == std::string::npos) return {};
    p += pat.size();
    if (p < line.size() && line[p] == '"') {
        std::string out;
        for (++p; p < line.size() && line[p] != '"'; ++p) {
            if (line[p] == '\\' && p + 1 < line.size()) {
                char e = line[++p];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else if (e == 'u' && p + 4 < line.size()) {
                    unsigned cp = static_cast<unsigned>(std::strtoul(line.substr(p + 1, 4).c_str(), nullptr, 16));
                    p += 4;
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) { out += static_cast<char>(0xC0 | cp >> 6); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else { out += static_cast<char>(0xE0 | cp >> 12); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                } else out += e;
            } else out += line[p];
        }
        return out;
    }
    size_t e = p;
    while (e < line.size() && line[e] != ',' && line[e] != '}') ++e;
    return line.substr(p, e - p);
}

// Runs argv (no shell), captures stdout. Returns the exit code, -1 if it cannot run.
int run_capture(const std::vector<std::string>& argv, std::string* out) {
    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    close(fd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) if (out) out->append(buf, static_cast<size_t>(n));
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Connects to the host socket. -1: nobody there (missing file / refused); -2: the socket exists but the host does
// not accept (backlog full: it is wedged). The connect is non-blocking so a wedged host can never hang us.
int try_connect(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        const int e = errno;
        ::close(fd);
        return e == EAGAIN ? -2 : -1;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

using Clock = std::chrono::steady_clock;

// One line from fd. 1: got it; 0: deadline passed (null deadline = wait forever); -1: EOF or error.
int read_line(int fd, std::string& buf, std::string& line, const Clock::time_point* deadline) {
    for (;;) {
        if (size_t p = buf.find('\n'); p != std::string::npos) {
            line = buf.substr(0, p);
            buf.erase(0, p + 1);
            return 1;
        }
        int ms = -1;
        if (deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now()).count();
            ms = left < 0 ? 0 : static_cast<int>(std::min<long long>(left, 1 << 30));
        }
        pollfd pf{fd, POLLIN, 0};
        const int r = ::poll(&pf, 1, ms);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        char tmp[4096];
        const ssize_t n = ::read(fd, tmp, sizeof tmp);
        if (n <= 0) return -1;
        buf.append(tmp, static_cast<size_t>(n));
    }
}

bool send_all(int fd, const std::string& l) {
    size_t off = 0;
    while (off < l.size()) {
        ssize_t n = ::send(fd, l.data() + off, l.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

struct HostInfo {
    bool alive = false;       // answered the ping
    bool hello = false;       // also sent hello (hosts from before the handshake only pong)
    std::string build;
    int pid = 0, windows = 0;
    long beat_ms = 0;         // age of the main-loop heartbeat
};

// hello + ping, both answered by the host's socket-reader thread (not its render loop), so a healthy host
// replies in microseconds. Anything that does not reply within `timeout` is wedged.
HostInfo handshake(int fd, std::string& buf, std::chrono::milliseconds timeout) {
    HostInfo h;
    if (!send_all(fd, "{\"cmd\":\"hello\"}\n{\"cmd\":\"ping\"}\n")) return h;
    const auto deadline = Clock::now() + timeout;
    std::string line;
    while (read_line(fd, buf, line, &deadline) == 1) {
        const std::string ev = field(line, "ev");
        if (ev == "hello") {
            h.hello = true;
            h.build = field(line, "build");
            h.pid = std::atoi(field(line, "pid").c_str());
            h.windows = std::atoi(field(line, "windows").c_str());
            h.beat_ms = std::atol(field(line, "beat_ms").c_str());
        } else if (ev == "pong") { h.alive = true; break; }
    }
    return h;
}

int peer_pid(int fd) {
    ucred c{};
    socklen_t n = sizeof c;
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &n) == 0 ? static_cast<int>(c.pid) : 0;
}

int pid_from_file(const std::string& dir) {
    FILE* f = std::fopen((dir + "/host.pid").c_str(), "r");
    if (!f) return 0;
    int p = 0;
    if (std::fscanf(f, "%d", &p) != 1) p = 0;
    std::fclose(f);
    return p;
}

bool pid_alive(int pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

bool wait_gone(int pid, std::chrono::milliseconds t) {
    const auto end = Clock::now() + t;
    while (pid_alive(pid)) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

// Every process below `pid` (CEF zygote, GPU, renderers), found by walking /proc ppids.
std::vector<int> descendants(int pid) {
    std::map<int, int> parent;
    if (DIR* d = ::opendir("/proc")) {
        while (dirent* e = ::readdir(d)) {
            const int p = std::atoi(e->d_name);
            if (p <= 0) continue;
            FILE* f = std::fopen(("/proc/" + std::to_string(p) + "/stat").c_str(), "r");
            if (!f) continue;
            char buf[1024];
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            buf[n] = 0;
            const char* rp = std::strrchr(buf, ')');
            int pp = 0;
            char st;
            if (rp && std::sscanf(rp + 1, " %c %d", &st, &pp) == 2) parent[p] = pp;
        }
        ::closedir(d);
    }
    std::vector<int> out, todo = {pid};
    while (!todo.empty()) {
        const int c = todo.back();
        todo.pop_back();
        for (auto& [p, pp] : parent) if (pp == c) { out.push_back(p); todo.push_back(p); }
    }
    return out;
}

// SIGTERM, ~1 s grace, SIGKILL; the host's leftover children go too. Returns true when it is gone.
bool kill_host(int pid) {
    if (pid <= 1 || pid == ::getpid()) return false;
    applog::log("terminating stuck luxium host pid " + std::to_string(pid));
    const std::vector<int> kids = descendants(pid);
    ::kill(pid, SIGTERM);
    if (!wait_gone(pid, 1000ms)) {
        applog::log("host ignored SIGTERM, sending SIGKILL");
        ::kill(pid, SIGKILL);
        wait_gone(pid, 2000ms);
    }
    for (int k : kids) ::kill(k, SIGKILL);
    return !pid_alive(pid);
}

// Identity of a luxium executable: size and mtime, the same pair the host reports for /proc/self/exe.
std::string binary_build(const std::string& bin) {
    struct stat st;
    if (bin.empty() || ::stat(bin.c_str(), &st) != 0) return {};
    return std::to_string(static_cast<long long>(st.st_size)) + "-" + std::to_string(static_cast<long long>(st.st_mtime));
}

// Detached host: double fork + setsid, stdio to host.log, so it outlives this app and is nobody's child.
void spawn_host(const std::string& bin, const std::string& dir) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        int log = ::open((dir + "/host.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        int nul = ::open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        if (log >= 0) { dup2(log, 1); dup2(log, 2); }
        for (int f = 3; f < 256; ++f) ::close(f);
        execl(bin.c_str(), bin.c_str(), "--host", static_cast<char*>(nullptr));
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}

} // namespace

// The menu item's action route, POSTed to the app's own server from here: own-engine windows run no JavaScript,
// so the old `eval fetch(...)` through the host is not available (and this works the same for Chromium windows).
void LuxiumWindow::post_loopback(int port, const std::string& path) {
    if (port <= 0 || path.empty() || path[0] != '/') return;
    std::thread([port, path] {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
            const std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                    "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) > 0) {
                char buf[512];
                while (::read(fd, buf, sizeof buf) > 0) {}   // drain: the route has run when the server closes
            }
        }
        ::close(fd);
    }).detach();
}

std::string LuxiumWindow::find_binary() {
    std::error_code ec;
    if (const char* e = std::getenv("LUXIUM_BIN"); e && *e) return fs::exists(e, ec) ? e : "";
    if (const char* p = std::getenv("PATH")) {
        std::string path = p;
        for (size_t i = 0; i <= path.size();) {
            size_t j = path.find(':', i);
            if (j == std::string::npos) j = path.size();
            fs::path c = fs::path(path.substr(i, j - i)) / "luxium";
            if (fs::is_regular_file(c, ec) && access(c.c_str(), X_OK) == 0) return c.string();
            i = j + 1;
        }
    }
    if (const char* h = std::getenv("HOME")) {
        fs::path c = fs::path(h) / ".local/opt/luxium/luxium";
        if (fs::is_regular_file(c, ec) && access(c.c_str(), X_OK) == 0) return c.string();
    }
    {
        fs::path c = "/opt/luxium/luxium";
        if (fs::is_regular_file(c, ec) && access(c.c_str(), X_OK) == 0) return c.string();
    }
    return "";
}

bool LuxiumWindow::available() {
    if (const char* b = std::getenv("LUX_DESKTOP_BACKEND"); b && std::string(b) == "webkit") return false;
    const char* d = std::getenv("DISPLAY");   // the host draws through X11 (XWayland is fine)
    return d && *d && !find_binary().empty();
}

LuxiumWindow::LuxiumWindow(WindowOptions opts) : opts_(std::move(opts)) {
    last_width_  = opts_.width;
    last_height_ = opts_.height;
}

LuxiumWindow::~LuxiumWindow() {
    { std::lock_guard<std::mutex> lk(tray_mutex_); tray_.reset(); }
    if (fd_ >= 0) ::close(fd_);
}

bool LuxiumWindow::send(const std::string& json) {
    std::lock_guard<std::mutex> lk(send_mutex_);
    if (fd_ < 0) return false;
    std::string l = json + "\n";
    size_t off = 0;
    while (off < l.size()) {
        ssize_t n = ::send(fd_, l.data() + off, l.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Returns a connection to a verified-healthy host (handshake done), starting or replacing one as needed.
// The whole decision runs under spawn.lock, so apps starting together cannot kill each other's fresh host.
//   - nobody listening: spawn one (cold start allows 20 s to bind, then a longer first handshake)
//   - host does not answer hello/ping in 2 s, or its render loop has not ticked for 10 s: it is wedged,
//     so SIGTERM / SIGKILL it (pid from SO_PEERCRED, else host.pid), drop the stale socket, spawn a fresh one, once
//   - host runs another luxium build than the one on disk and has no windows: ask it to `quit`, spawn the new
//     one; with windows it keeps serving them and only the skew is logged
// -1 on failure, with error_ set.
int LuxiumWindow::acquire_host() {
    const std::string dir = runtime_dir();
    const std::string sock = dir + "/host.sock";
    ::mkdir(dir.c_str(), 0700);
    int lk = ::open((dir + "/spawn.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lk >= 0) ::flock(lk, LOCK_EX);
    struct Unlock { int fd; ~Unlock() { if (fd >= 0) ::close(fd); } } unlock{lk};

    const std::string bin = find_binary();
    const std::string want_build = binary_build(bin);
    bool recovered = false, restarted = false;
    std::string last = "the Luxium host could not be reached";
    for (int round = 0; round < 3; ++round) {
        bool spawned = false;
        int fd = try_connect(sock);
        if (fd < 0) {
            if (fd == -2) {   // exists, never accepts
                applog::log("host socket does not accept connections");
                const int pid = pid_from_file(dir);
                if (!recovered && pid > 0) { recovered = true; kill_host(pid); ::unlink(sock.c_str()); continue; }
                last = "the Luxium host is not accepting connections";
                break;
            }
            if (bin.empty()) { last = "the luxium binary was not found (set LUXIUM_BIN, put it on PATH, or install it in ~/.local/opt/luxium)"; break; }
            applog::log("no host running, starting " + bin);
            spawn_host(bin, dir);
            spawned = true;
            const auto deadline = Clock::now() + 20s;   // cold start: SDL + socket
            while (fd < 0 && Clock::now() < deadline) {
                fd = try_connect(sock);
                if (fd < 0) std::this_thread::sleep_for(5ms);
            }
            if (fd < 0) { last = "the Luxium host did not start (see " + dir + "/host.log)"; break; }
        }
        std::string buf;
        HostInfo h = handshake(fd, buf, spawned ? 8000ms : 2000ms);
        if (!h.alive || (h.hello && h.beat_ms > 10000)) {
            applog::log(h.alive ? "host render loop stalled for " + std::to_string(h.beat_ms) + " ms" : "host did not answer hello/ping within the timeout");
            int pid = h.pid ? h.pid : peer_pid(fd);
            if (!pid) pid = pid_from_file(dir);
            ::close(fd);
            if (recovered || spawned) { last = "the Luxium host is not responding (see " + dir + "/host.log)"; break; }
            recovered = true;
            if (pid > 0) kill_host(pid);
            ::unlink(sock.c_str());
            ::unlink((dir + "/host.pid").c_str());
            continue;
        }
        if (!h.hello) applog::log("host predates the handshake: build and window count unknown");
        else if (!want_build.empty() && h.build != want_build) {
            applog::log("host build " + h.build + " differs from installed " + want_build + ", windows=" + std::to_string(h.windows));
            if (!restarted && h.windows == 0) {
                restarted = true;
                send_all(fd, "{\"cmd\":\"quit\"}\n");
                const auto deadline = Clock::now() + 1s;
                std::string line;
                bool bye = false;
                while (read_line(fd, buf, line, &deadline) == 1) if (field(line, "ev") == "bye") { bye = true; break; }
                ::close(fd);
                if (bye) {
                    applog::log("idle host with an old build asked to exit");
                    if (!wait_gone(h.pid, 5000ms)) kill_host(h.pid);
                    ::unlink(sock.c_str());
                    continue;
                }
                fd = try_connect(sock);   // refused (busy): keep using it
                if (fd < 0) continue;
            }
        }
        applog::log("host ok pid=" + std::to_string(h.pid) + " build=" + h.build + " windows=" + std::to_string(h.windows));
        return fd;
    }
    error_ = last;
    return -1;
}

void LuxiumWindow::run(const std::string& url) {
    if (size_t c = url.find("://127.0.0.1:"); c != std::string::npos) port_ = std::atoi(url.c_str() + c + 13);
    std::string icon = opts_.icon;
    if (!icon.empty()) { std::error_code ec; icon = fs::absolute(icon, ec).string(); }
    const std::string open =
        "{\"cmd\":\"open\",\"url\":" + jq(url) + ",\"title\":" + jq(opts_.title) +
        ",\"width\":" + std::to_string(opts_.width) + ",\"height\":" + std::to_string(opts_.height) +
        ",\"min_width\":" + std::to_string(std::min(opts_.width, 320)) + ",\"min_height\":" + std::to_string(std::min(opts_.height, 240)) +
        ",\"resizable\":" + (opts_.resizable ? "true" : "false") + ",\"dark\":" + (opts_.dark ? "true" : "false") +
        ",\"devtools\":" + (opts_.devtools ? "true" : "false") + ",\"icon\":" + jq(icon) +
        ",\"engine\":" + jq(opts_.engine) + ",\"app_id\":" + jq(opts_.app_id.empty() ? "app" : opts_.app_id) +
        ",\"wm_class\":" + jq(opts_.wm_class.empty() ? (opts_.app_id.empty() ? "app" : opts_.app_id) : opts_.wm_class) + "}";

    applog::log("opening window " + url);
    for (int attempt = 0; attempt < 3 && !stop_; ++attempt) {
        int fd = acquire_host();
        if (fd < 0) { failed_ = true; applog::log("giving up: " + error_); return; }
        { std::lock_guard<std::mutex> lk(send_mutex_); fd_ = fd; }
        if (!send(open)) { std::lock_guard<std::mutex> lk(send_mutex_); ::close(fd_); fd_ = -1; continue; }

        // A healthy host answers `open` with `opened` within moments; a cold Chromium start is the slow case.
        const auto open_deadline = Clock::now() + 15s;
        bool opened = false, fatal = false, timed_out = false;
        std::string buf, line;
        for (;;) {
            const int r = read_line(fd, buf, line, opened ? nullptr : &open_deadline);
            if (r == 0) { timed_out = true; break; }
            if (r < 0) break;   // host gone
            const std::string ev = field(line, "ev");
            if (ev == "opened") {
                opened = true;
                applog::log("window opened");
                std::string spec;
                { std::lock_guard<std::mutex> lk(send_mutex_); spec = menu_spec_; }
                if (!spec.empty()) send("{\"cmd\":\"set_menu\",\"spec\":" + jq(spec) + "}");
            } else if (ev == "menu") {
                // Same delivery as the WebKitGTK backend: the page POSTs the item's action route
                // ("?checked=true|false" appended for check items).
                std::string url = field(line, "action");
                const std::string checked = field(line, "checked");
                if (!url.empty()) {
                    if (!checked.empty()) url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("checked=") + checked;
                    post_loopback(port_, url);
                }
            } else if (ev == "resized") {
                last_width_  = std::atoi(field(line, "width").c_str());
                last_height_ = std::atoi(field(line, "height").c_str());
            } else if (ev == "result") {
                std::lock_guard<std::mutex> lk(res_mutex_);
                results_[std::atoi(field(line, "id").c_str())] = field(line, "value");
                res_cv_.notify_all();
            } else if (ev == "error") {
                applog::log("luxium host: " + field(line, "message"));
                if (!opened && field(line, "fatal") == "true") { error_ = "the Luxium host could not create the window: " + field(line, "message"); fatal = true; break; }
            } else if (ev == "closed") break;
        }
        { std::lock_guard<std::mutex> lk(send_mutex_); ::close(fd_); fd_ = -1; }
        if (opened || stop_) return;   // normal end
        if (fatal) { failed_ = true; return; }
        if (timed_out) {
            // Still pinging fine with a ticking render loop: just slow, do not disturb its other windows.
            applog::log("no 'opened' within 15 s");
            const std::string dir = runtime_dir();
            int pfd = try_connect(dir + "/host.sock");
            std::string pbuf;
            HostInfo h;
            if (pfd >= 0) { h = handshake(pfd, pbuf, 2000ms); ::close(pfd); }
            if (h.alive && (!h.hello || h.beat_ms < 10000)) {
                error_ = "the Luxium host did not open the window in 15 s (see " + dir + "/host.log)";
                failed_ = true;
                return;
            }
            const int pid = h.pid ? h.pid : pid_from_file(dir);
            if (pid > 0) kill_host(pid);
            ::unlink((dir + "/host.sock").c_str());
        }
        // EOF before "opened": the host was going away; connect again (acquire_host spawns a fresh one)
    }
    if (!stop_) { error_ = "the Luxium host kept closing the connection"; failed_ = true; applog::log(error_); }
}

void LuxiumWindow::terminate() {
    stop_ = true;
    send("{\"cmd\":\"close\"}");
    std::lock_guard<std::mutex> lk(send_mutex_);
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RD);   // wakes run() even if the host never answers
}

void LuxiumWindow::reload() { send("{\"cmd\":\"reload\"}"); }
void LuxiumWindow::set_title(const std::string& t) { send("{\"cmd\":\"set_title\",\"title\":" + jq(t) + "}"); }
void LuxiumWindow::minimize()   { send("{\"cmd\":\"minimize\"}"); }
void LuxiumWindow::maximize()   { send("{\"cmd\":\"maximize\"}"); }
void LuxiumWindow::restore()    { send("{\"cmd\":\"restore\"}"); }
void LuxiumWindow::fullscreen() { send("{\"cmd\":\"fullscreen\",\"on\":true}"); }
void LuxiumWindow::unfullscreen() { send("{\"cmd\":\"fullscreen\",\"on\":false}"); }
void LuxiumWindow::set_always_on_top(bool on) { send(std::string("{\"cmd\":\"always_on_top\",\"on\":") + (on ? "true}" : "false}")); }

// File dialogs run here, in the app's process: zenity (GTK) is what Luxium itself uses for <input type=file>.
std::string LuxiumWindow::pick(const std::string& name, bool save_mode, bool folder) {
    std::vector<std::string> a = {"zenity", "--file-selection", "--title=" + opts_.title};
    if (folder) a.push_back("--directory");
    if (save_mode) { a.push_back("--save"); a.push_back("--confirm-overwrite"); }
    if (!name.empty()) a.push_back("--filename=" + name);
    std::string out;
    if (run_capture(a, &out) != 0) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
std::string LuxiumWindow::pick_file(const std::string& name, bool save_mode) { return pick(name, save_mode, false); }
std::string LuxiumWindow::pick_folder() { return pick("", false, true); }

// freedesktop notification through notify-send (same D-Bus service the GTK build talked to directly).
void LuxiumWindow::notify(const std::string& title, const std::string& body) {
    pid_t pid = fork();
    if (pid != 0) { if (pid > 0) { int st; waitpid(pid, &st, 0); } return; }
    if (fork() != 0) _exit(0);   // double fork: no zombie, never blocks the caller
    execlp("notify-send", "notify-send", "-a", opts_.title.c_str(), title.c_str(), body.c_str(), static_cast<char*>(nullptr));
    _exit(127);
}

void LuxiumWindow::set_menu(const lux_script::Value& spec) {
    const std::string json = spec.to_json_text();
    { std::lock_guard<std::mutex> lk(send_mutex_); menu_spec_ = json; }
    send("{\"cmd\":\"set_menu\",\"spec\":" + jq(json) + "}");
}

// Left click toggles the window like the WebKitGTK tray does; the tray's own menu adds Quit.
void LuxiumWindow::set_tray(const std::string& icon_path, const std::string& tooltip) {
    std::error_code ec;
    const std::string icon = icon_path.empty() ? icon_path : fs::absolute(icon_path, ec).string();
    std::lock_guard<std::mutex> lk(tray_mutex_);
    if (!tray_)
        tray_ = std::make_unique<SniTray>(opts_.app_id.empty() ? "app" : opts_.app_id, opts_.title,
                                          [this] { send("{\"cmd\":\"toggle\"}"); }, [this] { terminate(); });
    tray_->set(icon, tooltip);
}

std::string LuxiumWindow::clipboard_read() {
    const int id = next_id_++;
    if (!send("{\"cmd\":\"clipboard_read\",\"id\":" + std::to_string(id) + "}")) return {};
    std::unique_lock<std::mutex> lk(res_mutex_);
    res_cv_.wait_for(lk, 2s, [&] { return results_.count(id) > 0; });
    auto it = results_.find(id);
    if (it == results_.end()) return {};
    std::string v = std::move(it->second);
    results_.erase(it);
    return v;
}
void LuxiumWindow::clipboard_write(const std::string& text) { send("{\"cmd\":\"clipboard_write\",\"text\":" + jq(text) + "}"); }
#endif
