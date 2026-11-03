#pragma once
// The platform-independent half of the Android shell: unpack the embedded resources and run the app's
// Lux server on loopback. This Lux version runs apps through its own main() (vendor/lux main.cpp,
// compiled with LUX_EMBEDDED -- the same entry the desktop shell uses), on a thread of this process.
// (Mail and Lux-Local have a different Lux and a boot.cpp built on lux::App; same interface.)
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>

class LuxServer {
public:
    // Extracts the embedded resources into work_dir (persistent: ./data/ and state.db live there) and
    // makes it the CWD. Throws std::runtime_error if it cannot.
    explicit LuxServer(const std::filesystem::path& work_dir);

    // Starts serving on 127.0.0.1:<free port> in a background thread and returns that port once it
    // accepts connections, or -1 if the app failed to start (see the log: its compile errors).
    int start();

    // Asks the server to drain (SIGTERM, the path Lux already handles) and joins.
    void stop();

private:
    std::thread       thread_;
    std::atomic<bool> exited_{false};
};
