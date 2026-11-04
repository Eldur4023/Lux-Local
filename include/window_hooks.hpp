#pragma once
// State and `window` LuxScript hooks shared by the release runtime (runtime.cpp) and dev mode (dev.cpp).
#include <atomic>
#include <mutex>
#include <string>

#include <lux_script/value.hpp>
#include <lux_script/window_control.hpp>

#include "window_backend.hpp"

// Set right before the window is force-closed by an external signal (see
// DesktopWindow::terminate() below) -- tells apart "the user closed the
// window" (nothing else knows yet, we raise SIGTERM ourselves) from "a
// signal already started the shutdown" (raising a second one would hit
// app.cpp's own two-signal "Forced exit" path on a shutdown already in
// progress).
inline std::atomic<bool> g_shutdown_from_signal{false};
inline WindowBackend* g_window = nullptr;
inline std::mutex g_window_mutex;

// Backs the `window` LuxScript module's callable functions
// (window.minimize(), window.open_file()...) -- see
// the window module and window_control.hpp.
// Every hook grabs g_window under the lock and releases it before doing
// anything that might block (pick_file can wait on the user for a while),
// so a slow file dialog never holds up set_title()/minimize()/etc.
inline void install_window_control_hooks() {
    auto& ctl = lux_script::window_control();
    ctl.set_title = [](const std::string& title) {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->set_title(title);
    };
    ctl.minimize = [] {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->minimize();
    };
    ctl.maximize = [] {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->maximize();
    };
    ctl.restore = [] {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->restore();
    };
    ctl.close = [] {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->terminate();
    };
    ctl.fullscreen = [] {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->fullscreen();
    };
    ctl.unfullscreen = [] {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->unfullscreen();
    };
    ctl.set_always_on_top = [](bool on_top) {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->set_always_on_top(on_top);
    };
    ctl.pick_file = [](const std::string& suggested_name, bool save_mode) -> std::string {
        WindowBackend* w;
        {
            std::lock_guard<std::mutex> lk(g_window_mutex);
            w = g_window;
        }
        return w ? w->pick_file(suggested_name, save_mode) : std::string();
    };
    ctl.pick_folder = []() -> std::string {
        WindowBackend* w;
        {
            std::lock_guard<std::mutex> lk(g_window_mutex);
            w = g_window;
        }
        return w ? w->pick_folder() : std::string();
    };
    ctl.notify = [](const std::string& title, const std::string& body) {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->notify(title, body);
    };
    ctl.set_menu = [](const lux_script::Value& spec) {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->set_menu(spec);
    };
    ctl.set_tray = [](const std::string& icon_path, const std::string& tooltip) {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->set_tray(icon_path, tooltip);
    };
    ctl.clipboard_read = []() -> std::string {
        WindowBackend* w;
        {
            std::lock_guard<std::mutex> lk(g_window_mutex);
            w = g_window;
        }
        return w ? w->clipboard_read() : std::string();
    };
    ctl.clipboard_write = [](const std::string& text) {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_window) g_window->clipboard_write(text);
    };
}

