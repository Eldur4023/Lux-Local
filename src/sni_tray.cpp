#ifdef LUXDESKTOP_LUXIUM
#include "sni_tray.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <unistd.h>

#include <mutex>
#include <thread>

namespace {

const char* kItemXml = R"XML(<node>
 <interface name="org.kde.StatusNotifierItem">
  <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
  <signal name="NewIcon"/><signal name="NewToolTip"/><signal name="NewStatus"><arg type="s"/></signal>
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="WindowId" type="i" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="IconPixmap" type="a(iiay)" access="read"/>
  <property name="OverlayIconName" type="s" access="read"/>
  <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
  <property name="AttentionIconName" type="s" access="read"/>
  <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
  <property name="AttentionMovieName" type="s" access="read"/>
  <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="Menu" type="o" access="read"/>
 </interface></node>)XML";

const char* kMenuXml = R"XML(<node>
 <interface name="com.canonical.dbusmenu">
  <method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
   <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
  <method name="GetGroupProperties"><arg type="ai" direction="in"/><arg type="as" direction="in"/><arg type="a(ia{sv})" direction="out"/></method>
  <method name="GetProperty"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
  <method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
  <method name="EventGroup"><arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/></method>
  <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
  <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
  <property name="Version" type="u" access="read"/>
  <property name="TextDirection" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconThemePath" type="as" access="read"/>
 </interface></node>)XML";

constexpr int kSizes[] = {22, 32, 48};

// PNG/anything GdkPixbuf reads -> a(iiay), ARGB32 in network byte order (what the spec asks for).
GVariant* pixmaps(const std::string& path) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
    if (!path.empty())
        for (int sz : kSizes) {
            GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_size(path.c_str(), sz, sz, nullptr);
            if (!pb) break;
            const int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), n = gdk_pixbuf_get_n_channels(pb);
            const int rs = gdk_pixbuf_get_rowstride(pb);
            const guchar* src = gdk_pixbuf_read_pixels(pb);
            GByteArray* out = g_byte_array_sized_new((guint)(w * h * 4));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const guchar* px = src + y * rs + x * n;
                    const guchar argb[4] = {n == 4 ? px[3] : (guchar)255, px[0], px[1], px[2]};
                    g_byte_array_append(out, argb, 4);
                }
            GVariant* data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, out->data, out->len, 1);
            g_variant_builder_add(&b, "(ii@ay)", w, h, data);
            g_byte_array_free(out, TRUE);
            g_object_unref(pb);
        }
    return g_variant_builder_end(&b);
}

} // namespace

struct SniTray::Impl {
    std::string id, title, icon, tooltip;
    std::function<void()> on_toggle, on_quit;
    std::mutex m;
    std::thread th;
    GMainContext* ctx = nullptr;
    GMainLoop* loop = nullptr;
    GDBusConnection* bus = nullptr;
    guint item_reg = 0, menu_reg = 0, owner = 0, watch = 0;
    std::string name;

    void register_with_watcher() {
        g_dbus_connection_call(bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                               "RegisterStatusNotifierItem", g_variant_new("(s)", name.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 2000,
                               nullptr, nullptr, nullptr);
    }
    void emit(const char* sig) {
        if (bus) g_dbus_connection_emit_signal(bus, nullptr, "/StatusNotifierItem", "org.kde.StatusNotifierItem", sig, nullptr, nullptr);
    }

    static void item_call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant*,
                          GDBusMethodInvocation* inv, gpointer self) {
        auto* i = static_cast<Impl*>(self);
        const std::string m = method;
        if (m == "Activate" || m == "SecondaryActivate") if (i->on_toggle) i->on_toggle();
        g_dbus_method_invocation_return_value(inv, nullptr);   // ContextMenu/Scroll: the dbusmenu covers it
    }
    static GVariant* item_get(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* prop, GError**, gpointer self) {
        auto* i = static_cast<Impl*>(self);
        std::lock_guard<std::mutex> lk(i->m);
        const std::string p = prop;
        if (p == "Category") return g_variant_new_string("ApplicationStatus");
        if (p == "Id") return g_variant_new_string(i->id.c_str());
        if (p == "Title") return g_variant_new_string(i->title.c_str());
        if (p == "Status") return g_variant_new_string("Active");
        if (p == "WindowId") return g_variant_new_int32(0);
        if (p == "IconPixmap") return pixmaps(i->icon);
        if (p == "OverlayIconPixmap" || p == "AttentionIconPixmap") return pixmaps("");
        if (p == "IconName" || p == "OverlayIconName" || p == "AttentionIconName" || p == "AttentionMovieName") return g_variant_new_string("");
        if (p == "ToolTip") return g_variant_new("(s@a(iiay)ss)", "", pixmaps(""), i->tooltip.c_str(), "");
        if (p == "ItemIsMenu") return g_variant_new_boolean(FALSE);
        if (p == "Menu") return g_variant_new_object_path("/MenuBar");
        return nullptr;
    }

    // dbusmenu: 0 root, 1 "Show / Hide", 2 "Quit"
    static GVariant* node(int id, const char* label) {
        GVariantBuilder p;
        g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
        if (id) {
            g_variant_builder_add(&p, "{sv}", "label", g_variant_new_string(label));
            g_variant_builder_add(&p, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(&p, "{sv}", "visible", g_variant_new_boolean(TRUE));
        } else g_variant_builder_add(&p, "{sv}", "children-display", g_variant_new_string("submenu"));
        return g_variant_new("(ia{sv}av)", id, &p, nullptr);
    }
    static void menu_call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* params,
                          GDBusMethodInvocation* inv, gpointer self) {
        auto* i = static_cast<Impl*>(self);
        const std::string m = method;
        if (m == "GetLayout") {
            GVariantBuilder kids;
            g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
            g_variant_builder_add(&kids, "v", node(1, "Show / Hide"));
            g_variant_builder_add(&kids, "v", node(2, "Quit"));
            GVariantBuilder p;
            g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&p, "{sv}", "children-display", g_variant_new_string("submenu"));
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u(ia{sv}av))", 1u, 0, &p, &kids));
        } else if (m == "Event") {
            gint32 id = 0;
            const char* ev = "";
            g_variant_get(params, "(i&svu)", &id, &ev, nullptr, nullptr);
            if (std::string(ev) == "clicked") {
                if (id == 1 && i->on_toggle) i->on_toggle();
                if (id == 2 && i->on_quit) i->on_quit();
            }
            g_dbus_method_invocation_return_value(inv, nullptr);
        } else if (m == "AboutToShow") {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
        } else if (m == "GetGroupProperties") {
            GVariantBuilder out;
            g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
            for (int id : {1, 2}) {
                GVariantBuilder p;
                g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&p, "{sv}", "label", g_variant_new_string(id == 1 ? "Show / Hide" : "Quit"));
                g_variant_builder_add(&out, "(ia{sv})", id, &p);
            }
            g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &out));
        } else if (m == "EventGroup") {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(ai)", nullptr));
        } else g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "unsupported");
    }
    static GVariant* menu_get(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* prop, GError**, gpointer) {
        const std::string p = prop;
        if (p == "Version") return g_variant_new_uint32(3);
        if (p == "TextDirection") return g_variant_new_string("ltr");
        if (p == "Status") return g_variant_new_string("normal");
        if (p == "IconThemePath") return g_variant_new_strv(nullptr, 0);
        return nullptr;
    }

    void run() {
        g_main_context_push_thread_default(ctx);
        GError* err = nullptr;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
        if (!bus) { g_clear_error(&err); return; }   // no session bus: no tray, never fatal
        static const GDBusInterfaceVTable item_vt = {item_call, item_get, nullptr, {}};
        static const GDBusInterfaceVTable menu_vt = {menu_call, menu_get, nullptr, {}};
        GDBusNodeInfo* ni = g_dbus_node_info_new_for_xml(kItemXml, nullptr);
        GDBusNodeInfo* nm = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
        item_reg = g_dbus_connection_register_object(bus, "/StatusNotifierItem", ni->interfaces[0], &item_vt, this, nullptr, nullptr);
        menu_reg = g_dbus_connection_register_object(bus, "/MenuBar", nm->interfaces[0], &menu_vt, this, nullptr, nullptr);
        name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
        owner = g_bus_own_name_on_connection(bus, name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);
        // Register now and every time a (new) watcher shows up on the bus.
        watch = g_bus_watch_name_on_connection(bus, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, gpointer self) { static_cast<Impl*>(self)->register_with_watcher(); },
            nullptr, this, nullptr);
        g_main_loop_run(loop);
        g_bus_unwatch_name(watch);
        g_bus_unown_name(owner);
        g_dbus_connection_unregister_object(bus, item_reg);
        g_dbus_connection_unregister_object(bus, menu_reg);
        g_dbus_node_info_unref(ni);
        g_dbus_node_info_unref(nm);
        g_object_unref(bus);
        bus = nullptr;
        g_main_context_pop_thread_default(ctx);
    }
};

SniTray::SniTray(std::string id, std::string title, std::function<void()> on_toggle, std::function<void()> on_quit) : p_(new Impl) {
    p_->id = std::move(id);
    p_->title = std::move(title);
    p_->on_toggle = std::move(on_toggle);
    p_->on_quit = std::move(on_quit);
    p_->ctx = g_main_context_new();
    p_->loop = g_main_loop_new(p_->ctx, FALSE);
    p_->th = std::thread([this] { p_->run(); });
}

SniTray::~SniTray() {
    g_main_loop_quit(p_->loop);   // thread-safe
    if (p_->th.joinable()) p_->th.join();
    g_main_loop_unref(p_->loop);
    g_main_context_unref(p_->ctx);
    delete p_;
}

void SniTray::set(const std::string& icon_path, const std::string& tooltip) {
    { std::lock_guard<std::mutex> lk(p_->m); p_->icon = icon_path; p_->tooltip = tooltip; }
    g_main_context_invoke(p_->ctx, [](gpointer self) -> gboolean {
        auto* i = static_cast<Impl*>(self);
        i->emit("NewIcon");
        i->emit("NewToolTip");
        return G_SOURCE_REMOVE;
    }, p_);
}
#endif
