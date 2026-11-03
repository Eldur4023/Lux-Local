#pragma once
// Per-app launch log and error dialog, so a start that goes wrong leaves a trace and tells the user.
//   log:    $XDG_STATE_HOME or ~/.local/state/lux-desktop/<app>.log, rotated to <app>.log.1 at 1 MB
//   dialog: zenity --error, else kdialog, else stderr only
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

namespace applog {

inline std::string& app_name() { static std::string a = "app"; return a; }
inline void set_app(const std::string& a) { app_name() = a.empty() ? "app" : a; }

inline std::string path() {
    namespace fs = std::filesystem;
    const char* x = std::getenv("XDG_STATE_HOME");
    const char* h = std::getenv("HOME");
    fs::path dir = (x && *x) ? fs::path(x) : fs::path(h ? h : "/tmp") / ".local/state";
    dir /= "lux-desktop";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / (app_name() + ".log")).string();
}

inline void log(const std::string& msg) {
    static std::mutex m;
    static bool rotated = false;
    std::lock_guard<std::mutex> lk(m);
    const std::string p = path();
    if (!rotated) {   // once per run: keep the file bounded at about 1 MB
        rotated = true;
        struct stat st;
        if (::stat(p.c_str(), &st) == 0 && st.st_size > (1 << 20)) std::rename(p.c_str(), (p + ".1").c_str());
    }
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    char line[64];
    std::snprintf(line, sizeof line, "%s.%03d [%d] ", ts, (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000), (int)::getpid());
    const std::string out = std::string(line) + msg + "\n";
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) { (void)!::write(fd, out.data(), out.size()); ::close(fd); }
    std::fprintf(stderr, "luxdesktop: %s\n", msg.c_str());
}

// Shows `text` to the user (blocks until dismissed) and mentions the log. Never throws, never hangs on a missing tool.
inline void dialog_error(const std::string& text) {
    const std::string full = text + "\n\nLog: " + path();
    log("error dialog: " + text);
    const char* d = std::getenv("DISPLAY");
    const char* w = std::getenv("WAYLAND_DISPLAY");
    if ((!d || !*d) && (!w || !*w)) return;
    if (std::getenv("LUX_NO_DIALOG")) return;   // tests
    for (int tool = 0; tool < 2; ++tool) {
        pid_t pid = fork();
        if (pid < 0) return;
        if (pid == 0) {
            int nul = ::open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
            if (tool == 0) execlp("zenity", "zenity", "--error", "--no-markup", "--width=460", ("--title=" + app_name()).c_str(), ("--text=" + full).c_str(), (char*)nullptr);
            else execlp("kdialog", "kdialog", "--error", full.c_str(), "--title", app_name().c_str(), (char*)nullptr);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) != 127) return;
    }
}

} // namespace applog
#else
#include <iostream>
#include <string>
namespace applog {
inline void set_app(const std::string&) {}
inline std::string path() { return {}; }
inline void log(const std::string& m) { std::cerr << "luxdesktop: " << m << "\n"; }
inline void dialog_error(const std::string& m) { log(m); }
}
#endif
