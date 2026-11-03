#ifdef LUXDESKTOP_LUXIUM
#include "luxium_window.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
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

int try_connect(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) { ::close(fd); return -1; }
    return fd;
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

// Connects to the host; if none is up, starts one. The spawn lock makes concurrent cold starts of several
// apps end with exactly one host (the host also holds host.lock for its whole life, so a second one would
// just exit).
int LuxiumWindow::connect_host() {
    const std::string dir = runtime_dir();
    const std::string sock = dir + "/host.sock";
    if (int fd = try_connect(sock); fd >= 0) return fd;
    ::mkdir(dir.c_str(), 0700);
    int lk = ::open((dir + "/spawn.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lk >= 0) ::flock(lk, LOCK_EX);
    int fd = try_connect(sock);   // another app may have started it while we waited
    if (fd < 0) {
        const std::string bin = find_binary();
        if (!bin.empty()) spawn_host(bin, dir);
        const auto deadline = std::chrono::steady_clock::now() + 20s;   // cold start: CEF init
        while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
            fd = try_connect(sock);
            if (fd < 0) std::this_thread::sleep_for(5ms);
        }
    }
    if (lk >= 0) ::close(lk);   // releases the lock
    return fd;
}

void LuxiumWindow::run(const std::string& url) {
    std::string icon = opts_.icon;
    if (!icon.empty()) { std::error_code ec; icon = fs::absolute(icon, ec).string(); }
    const std::string open =
        "{\"cmd\":\"open\",\"url\":" + jq(url) + ",\"title\":" + jq(opts_.title) +
        ",\"width\":" + std::to_string(opts_.width) + ",\"height\":" + std::to_string(opts_.height) +
        ",\"min_width\":" + std::to_string(std::min(opts_.width, 320)) + ",\"min_height\":" + std::to_string(std::min(opts_.height, 240)) +
        ",\"resizable\":" + (opts_.resizable ? "true" : "false") + ",\"dark\":" + (opts_.dark ? "true" : "false") +
        ",\"devtools\":" + (opts_.devtools ? "true" : "false") + ",\"icon\":" + jq(icon) +
        ",\"app_id\":" + jq(opts_.app_id.empty() ? "app" : opts_.app_id) + "}";

    for (int attempt = 0; attempt < 3 && !stop_; ++attempt) {
        int fd = connect_host();
        if (fd < 0) { std::cerr << "luxdesktop: cannot reach the Luxium host\n"; return; }
        { std::lock_guard<std::mutex> lk(send_mutex_); fd_ = fd; }
        if (!send(open)) { std::lock_guard<std::mutex> lk(send_mutex_); ::close(fd_); fd_ = -1; continue; }

        bool opened = false;
        std::string buf;
        char tmp[4096];
        for (;;) {
            ssize_t n = ::read(fd, tmp, sizeof tmp);
            if (n <= 0) break;   // host gone
            buf.append(tmp, static_cast<size_t>(n));
            size_t p;
            bool closed = false;
            while ((p = buf.find('\n')) != std::string::npos) {
                const std::string line = buf.substr(0, p);
                buf.erase(0, p + 1);
                const std::string ev = field(line, "ev");
                if (ev == "opened") opened = true;
                else if (ev == "resized") {
                    last_width_  = std::atoi(field(line, "width").c_str());
                    last_height_ = std::atoi(field(line, "height").c_str());
                } else if (ev == "result") {
                    std::lock_guard<std::mutex> lk(res_mutex_);
                    results_[std::atoi(field(line, "id").c_str())] = field(line, "value");
                    res_cv_.notify_all();
                } else if (ev == "error") {
                    std::cerr << "luxdesktop: luxium host: " << field(line, "message") << "\n";
                } else if (ev == "closed") closed = true;
            }
            if (closed) break;
        }
        { std::lock_guard<std::mutex> lk(send_mutex_); ::close(fd_); fd_ = -1; }
        if (opened || stop_) return;   // normal end; EOF before "opened" = host was going away: retry
    }
}

void LuxiumWindow::terminate() {
    stop_ = true;
    send("{\"cmd\":\"close\"}");
    std::lock_guard<std::mutex> lk(send_mutex_);
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RD);   // wakes run() even if the host never answers
}

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

void LuxiumWindow::set_menu(const lux_script::Value&) {
    if (!menu_warned_) { menu_warned_ = true; std::cerr << "luxdesktop: window.set_menu() is not supported on the Luxium backend\n"; }
}
void LuxiumWindow::set_tray(const std::string&, const std::string&) {
    if (!menu_warned_) { menu_warned_ = true; std::cerr << "luxdesktop: window.set_tray() is not supported on the Luxium backend\n"; }
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
