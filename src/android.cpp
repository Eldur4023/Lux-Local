// Android shell: no window here. The Kotlin side owns a WebView and asks this
// library to run the app's Lux server on loopback; the WebView just loads the
// returned port, exactly as the GTK shell points WebKit at it (src/runtime.cpp).
//
//   object LuxLocal { external fun start(dataDir: String): Int   // port, or -1
//                     external fun stop()
//                     @JvmStatic fun secretGet/secretSet/secretDelete(...) }   // keyring backend, see below
//
// dataDir is context.filesDir: persistent, so ./data/ (SQLite...) survives restarts.
// The `window` module's hooks stay unset (window_control.hpp: every hook is optional),
// so window.set_title()/notify()... are no-ops until the Android side wires them.
#include "boot.hpp"

#if __has_include(<lux_script/keyring_control.hpp>)   // only Lux versions with the keyring module
#include <lux_script/keyring_control.hpp>
#define LUX_HAS_KEYRING 1
#endif
#include <lux_script/window_control.hpp>

#ifdef LUX_TZDATA   // apps whose modules need time zones (Calendar): date/tz library, see lux_script/tz.hpp
#include <date/tz.h>
#endif

#include <android/log.h>
#include <jni.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <mutex>

namespace {
std::mutex                 g_mutex;
std::unique_ptr<LuxServer> g_server;
int                        g_port = -1;

JavaVM* g_vm = nullptr;
jclass  g_cls = nullptr; // global ref: FindClass from a native thread would use the wrong class loader

// Calls a static LuxLocal method from any thread (the keyring hooks run on blocking_pool workers).
// Attached threads stay attached; the VM detaches them when the thread exits only if we ask, and
// blocking_pool threads live as long as the process, so leaving them attached is fine.
JNIEnv* env_here() {
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    return g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
}

std::string to_std(JNIEnv* env, jstring s) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

// secret(<method>, service, key, [value]) -> jstring result, or null; *ok = no Java exception.
jstring call_secret(JNIEnv* env, const char* method, const char* sig, const std::string& service,
                    const std::string& key, const std::string* value) {
    jmethodID m = env->GetStaticMethodID(g_cls, method, sig);
    jstring js = env->NewStringUTF(service.c_str()), jk = env->NewStringUTF(key.c_str());
    jstring jv = value ? env->NewStringUTF(value->c_str()) : nullptr;
    jobject r = value ? env->CallStaticObjectMethod(g_cls, m, js, jk, jv) : env->CallStaticObjectMethod(g_cls, m, js, jk);
    if (env->ExceptionCheck()) { env->ExceptionClear(); r = nullptr; }
    return static_cast<jstring>(r);
}

// Android keeps its CAs as one file per cert in /system/etc/security/cacerts, named with the OLD
// OpenSSL subject hash, which OpenSSL 3 (static, inside libcurl) does not look up: every TLS handshake
// fails with "unable to get local issuer certificate". So concatenate the PEM blocks into one bundle
// and point OpenSSL at it (libcurl is built with no compiled-in CA path, see tools/android-deps.sh).
void use_system_cas(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path bundle = dir / "cacerts.pem";
    std::ofstream out(bundle, std::ios::binary | std::ios::trunc);
    for (const auto& e : fs::directory_iterator("/system/etc/security/cacerts", ec)) {
        std::ifstream in(e.path());
        std::string line;
        bool pem = false;
        while (std::getline(in, line)) {
            if (line.rfind("-----BEGIN", 0) == 0) pem = true;
            if (pem) out << line << '\n';
            if (line.rfind("-----END", 0) == 0) pem = false;
        }
    }
    out.close();
    setenv("SSL_CERT_FILE", bundle.c_str(), 1);
}

// window.notify(title, body) -> LuxLocal.notify (a system notification). The other window hooks stay unset.
// A static LuxLocal method taking and returning strings (and optionally a boolean flag).
std::string call_string(const char* method, const char* sig, const std::string& a, bool flag, bool with_flag) {
    JNIEnv* env = env_here();
    if (!env) return "";
    jmethodID m = env->GetStaticMethodID(g_cls, method, sig);
    jstring ja = env->NewStringUTF(a.c_str());
    jobject r = with_flag ? env->CallStaticObjectMethod(g_cls, m, ja, static_cast<jboolean>(flag))
                          : env->CallStaticObjectMethod(g_cls, m, ja);
    env->DeleteLocalRef(ja);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    if (!r) return "";
    std::string out = to_std(env, static_cast<jstring>(r));
    env->DeleteLocalRef(r);
    return out;
}

// Hooks that only some Lux Desktop versions declare in WindowControl (a template, so the discarded
// branches are never instantiated for the versions that lack them).
// pick_file/pick_folder BLOCK the calling worker thread until the user answers an Activity (see LuxLocal.kt).
template <class Ctl>
void install_optional_hooks(Ctl& ctl) {
    if constexpr (requires { ctl.pick_file; })
        ctl.pick_file = [](const std::string& suggested_name, bool save_mode) {
            return call_string("pickFile", "(Ljava/lang/String;Z)Ljava/lang/String;", suggested_name, save_mode, true);
        };
    if constexpr (requires { ctl.pick_folder; })
        ctl.pick_folder = []() { return call_string("pickFolder", "(Ljava/lang/String;)Ljava/lang/String;", "", false, false); };
    if constexpr (requires { ctl.clipboard_write; })
        ctl.clipboard_write = [](const std::string& text) {
            call_string("clipboardWrite", "(Ljava/lang/String;)Ljava/lang/String;", text, false, false);
        };
}

// window.* -> Android.
void install_window_hooks() {
    auto& ctl = lux_script::window_control();
    ctl.notify = [](const std::string& title, const std::string& body) {
        JNIEnv* env = env_here();
        if (!env) return;
        jmethodID m = env->GetStaticMethodID(g_cls, "notify", "(Ljava/lang/String;Ljava/lang/String;)V");
        jstring jt = env->NewStringUTF(title.c_str()), jb = env->NewStringUTF(body.c_str());
        env->CallStaticVoidMethod(g_cls, m, jt, jb);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(jt);
        env->DeleteLocalRef(jb);
    };
    install_optional_hooks(ctl);
}

#ifdef LUX_HAS_KEYRING
void install_keyring_hooks() {
    auto& k = lux_script::keyring_control();
    k.get = [](const std::string& service, const std::string& key) -> std::optional<std::string> {
        JNIEnv* env = env_here();
        if (!env) return std::nullopt;
        jstring r = call_secret(env, "secretGet", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", service, key, nullptr);
        if (!r) return std::nullopt;
        return to_std(env, r);
    };
    k.set = [](const std::string& service, const std::string& key, const std::string& secret) {
        JNIEnv* env = env_here();
        return env && call_secret(env, "secretSet", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                   service, key, &secret) != nullptr; // returns "1" on success
    };
    k.del = [](const std::string& service, const std::string& key) {
        JNIEnv* env = env_here();
        return env && call_secret(env, "secretDelete", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", service, key, nullptr) != nullptr;
    };
}
#endif
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    g_cls = static_cast<jclass>(env->NewGlobalRef(env->FindClass("dev/lux/local/LuxLocal")));
#ifdef LUX_HAS_KEYRING
    install_keyring_hooks();
#endif
    install_window_hooks();
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_lux_local_LuxLocal_start(JNIEnv* env, jobject, jstring data_dir) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_server) return g_port; // already running (Activity recreated): one server per process, reuse it
    const char* dir = env->GetStringUTFChars(data_dir, nullptr);
    std::filesystem::path work_dir(dir);
    env->ReleaseStringUTFChars(data_dir, dir);
    try {
        std::filesystem::create_directories(work_dir);
        use_system_cas(work_dir);
#ifdef LUX_TZDATA
        date::set_install((work_dir / "tzdata").string());   // the IANA text files, extracted from the app's resources
#endif
        g_server = std::make_unique<LuxServer>(work_dir);
        g_port = g_server->start();
        if (g_port <= 0) { g_server->stop(); g_server.reset(); g_port = -1; }   // the app did not come up: allow another try
        return g_port;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "luxlocal", "%s", e.what());
        g_server.reset();
        return -1;
    }
}

// Environment for the app (e.g. CALENDAR_TZ): call before start().
extern "C" JNIEXPORT void JNICALL
Java_dev_lux_local_LuxLocal_putenv(JNIEnv* env, jobject, jstring key, jstring value) {
    const char* k = env->GetStringUTFChars(key, nullptr);
    const char* v = env->GetStringUTFChars(value, nullptr);
    setenv(k, v, 1);
    env->ReleaseStringUTFChars(key, k);
    env->ReleaseStringUTFChars(value, v);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_lux_local_LuxLocal_stop(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_server) return;
    g_server->stop();
    g_server.reset();
    g_port = -1;
}
