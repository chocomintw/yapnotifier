#include "menu.h"

#include <Windows.h>

#include <RmlUi/Core.h>

#include <string>
#include <vector>

#include "log.h"
#include "teamspeak.h"
#include "update.h"
#include "version.h"

namespace yap::menu {
namespace {
using Args = const Rml::VariantList&;

// Rows for the per-user / per-channel editors: the map nodes are stable, so the editor
// binds straight into the Config through the pointer (RmlUi follows getter pointers).
struct UserRow {
    std::string uid, label;
    bool present = false;
    UserOverride* p = nullptr;
    UserOverride* v() { return p; }
};
struct ChannelRow {
    std::string label;
    uint64_t id = 0;
    bool current = false;
    ChannelOverride* p = nullptr;
    ChannelOverride* v() { return p; }
};
struct Person {
    std::string uid, nickname;
    bool customised = false;
};

// Everything the Status/Users/Channel/Profiles tabs show that isn't a Config field.
struct Live {
    bool alive = false, legacy = false, channel_customised = false;
    int conn = 0;
    std::string server, channel, parent, count, notice, key_name, channel_id;
    std::vector<Person> people;
    std::vector<UserRow> users;
    std::vector<ChannelRow> channels;
    std::vector<std::string> profiles;
    std::string selected, new_profile, status, version = "YapNotifier v" YAP_VERSION;
    std::vector<std::string> ind_names, notif_names;
    std::vector<std::string> anchor_names{"Top left", "Top centre", "Top right", "Middle left", "Centre", "Middle right", "Bottom left", "Bottom centre", "Bottom right"};
    std::vector<std::string> sort_names{"Channel order", "Alphabetical", "Speaking first"};
    std::vector<std::string> icon_names{"None", "Dot", "Circle", "Ring", "Square", "Diamond", "Triangle", "Star", "Chevron", "Microphone", "Microphone muted", "Speaker", "Speaker muted", "Moon", "Record", "Crown", "Whisper", "Bars"};
};

Live g_live;
Rml::ElementDocument* g_doc = nullptr;
Rml::DataModelHandle g_model;

std::string key_name(int vk) {
    if (vk == VK_INSERT) return "INSERT";
    UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lp = static_cast<LONG>(scan << 16);
    switch (vk) {
        case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            lp |= 1 << 24;
    }
    wchar_t w[64] = {};
    if (GetKeyNameTextW(lp, w, 64) <= 0) return "key " + std::to_string(vk);
    char out[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof out, nullptr, nullptr);
    return out;
}

std::string pretty(const char* ini_name) {
    std::string s = ini_name;
    for (char& ch : s) ch = ch == '_' ? ' ' : ch;
    return s;
}

void save(Host& h) {
    config::save(h.cfg, h.ini);
    ts::configure(h.cfg.port);
    log::info("menu: config saved");
}

void reload(Host& h) {
    const bool demo = h.cfg.demo;
    h.cfg = config::load(h.ini);
    h.cfg.demo = demo;
    ts::configure(h.cfg.port);
}

std::shared_ptr<const ts::Snapshot> snapshot(const Host& h) {
    if (h.cfg.demo) return {&hud::demo_snapshot(), [](const ts::Snapshot*) {}};
    return ts::snapshot();
}

// Live facts + the override rows, rebuilt every frame the menu is open (a few entries).
void refresh(Host& h) {
    Live& l = g_live;
    auto snap = snapshot(h);
    l.alive = snap->alive;
    l.legacy = snap->legacy;
    l.conn = snap->conn;
    l.server = snap->server_name;
    l.channel = snap->channel;
    l.parent = snap->parent;
    l.count = std::to_string(snap->users.size());
    l.channel_id = std::to_string(snap->channel_id);
    l.channel_customised = h.cfg.channels.count(snap->channel_id) != 0;
    l.notice = *update::notice();
    l.key_name = key_name(h.cfg.menu_key);

    l.people.clear();
    for (const auto& u : snap->users) l.people.push_back({u.uid, u.nickname, h.cfg.users.count(u.uid) != 0});
    l.users.clear();
    for (auto& [uid, ov] : h.cfg.users) {
        UserRow r{uid, ov.display_name.empty() ? uid : ov.display_name + " (" + uid + ")", false, &ov};
        for (const auto& u : snap->users) r.present |= u.uid == uid;
        l.users.push_back(std::move(r));
    }
    if (!l.selected.empty() && !h.cfg.users.count(l.selected)) l.selected.clear();
    l.channels.clear();
    for (auto& [id, ov] : h.cfg.channels) {
        const bool cur = id == snap->channel_id;
        l.channels.push_back({"Channel " + std::to_string(id) + (cur ? " (current)" : ""), id, cur, &ov});
    }
    l.profiles = config::list_profiles(h.ini);
}
}  // namespace

bool init(Host& h, Rml::Context& ctx) {
    Config& c = h.cfg;
    Rml::DataModelConstructor m = ctx.CreateDataModel("cfg");
    if (!m) {
        log::error("menu: could not create the data model");
        return false;
    }

    // --- types -----------------------------------------------------------------------
    if (auto s = m.RegisterStruct<Indicator>()) {
        s.RegisterMember("enabled", &Indicator::enabled);
        s.RegisterMember("icon", &Indicator::icon);
        s.RegisterMember("icon_color", &Indicator::icon_color);
        s.RegisterMember("text_color", &Indicator::text_color);
        s.RegisterMember("dim", &Indicator::dim);
    }
    m.RegisterArray<std::array<Indicator, IndCount>>();
    if (auto s = m.RegisterStruct<NotifCategory>()) {
        s.RegisterMember("enabled", &NotifCategory::enabled);
        s.RegisterMember("format", &NotifCategory::format);
        s.RegisterMember("color", &NotifCategory::color);
        s.RegisterMember("hold_ms", &NotifCategory::hold_ms);
    }
    m.RegisterArray<std::array<NotifCategory, NotifCount>>();
    if (auto s = m.RegisterStruct<UserOverride>()) {
        s.RegisterMember("name_color", &UserOverride::name_color);
        s.RegisterMember("speaking_color", &UserOverride::speaking_color);
        s.RegisterMember("icon", &UserOverride::icon);
        s.RegisterMember("icon_color", &UserOverride::icon_color);
        s.RegisterMember("display_name", &UserOverride::display_name);
        s.RegisterMember("is_friend", &UserOverride::is_friend);
        s.RegisterMember("friend_tag", &UserOverride::friend_tag);
    }
    if (auto s = m.RegisterStruct<UserRow>()) {
        s.RegisterMember("uid", &UserRow::uid);
        s.RegisterMember("label", &UserRow::label);
        s.RegisterMember("present", &UserRow::present);
        s.RegisterMember("v", &UserRow::v);
    }
    m.RegisterArray<std::vector<UserRow>>();
    if (auto s = m.RegisterStruct<ChannelOverride>()) {
        s.RegisterMember("title_color", &ChannelOverride::title_color);
        s.RegisterMember("display_name", &ChannelOverride::display_name);
        s.RegisterMember("icon", &ChannelOverride::icon);
        s.RegisterMember("icon_color", &ChannelOverride::icon_color);
        s.RegisterMember("opacity", &ChannelOverride::opacity);
    }
    if (auto s = m.RegisterStruct<ChannelRow>()) {
        s.RegisterMember("label", &ChannelRow::label);
        s.RegisterMember("id", &ChannelRow::id);
        s.RegisterMember("current", &ChannelRow::current);
        s.RegisterMember("v", &ChannelRow::v);
    }
    m.RegisterArray<std::vector<ChannelRow>>();
    if (auto s = m.RegisterStruct<Person>()) {
        s.RegisterMember("uid", &Person::uid);
        s.RegisterMember("nickname", &Person::nickname);
        s.RegisterMember("customised", &Person::customised);
    }
    m.RegisterArray<std::vector<Person>>();
    m.RegisterArray<std::vector<std::string>>();

    // --- Config fields: menu.rml binds them by their ini names ----------------------------
#define B(field) m.Bind(#field, &c.field)
    B(port); B(auto_update); B(demo); B(menu_dark);
    B(show_when_disconnected); B(master_opacity); B(scale); B(font_file); B(font_size);
    B(text_shadow); B(text_outline); B(text_color); B(text_secondary);
    B(anchor); B(pos_x); B(pos_y); B(show_title); B(show_parent); B(show_count); B(show_server);
    B(title_format); B(disconnected_text); B(show_panel); B(panel_color); B(corner_radius);
    B(row_spacing); B(icon_size); B(title_color);
    B(only_show_talking); B(show_muted_users); B(show_local_user); B(highlight_local_user); B(local_user_color);
    B(sort); B(max_visible_users); B(show_overflow_count); B(max_name_width);
    B(color_friends); B(friend_color); B(show_friend_tag); B(friend_tag_color);
    B(ind);
    B(speaking_attack_ms); B(speaking_release_ms); B(pulse); B(pulse_hz);
    B(fade_when_idle); B(idle_after_ms); B(idle_opacity);
    B(notif_enabled); B(notif_anchor); B(notif_x); B(notif_y); B(notif_width); B(notif_max_visible);
    B(notif_stack_up); B(notif_in_ms); B(notif_out_ms); B(notif_merge_duplicates);
    B(notif_suppress_after_connect_ms); B(notif_background); B(notif_accent_bar); B(notif);
    B(chat_enabled); B(chat_anchor); B(chat_x); B(chat_y); B(chat_width);
    B(chat_channel); B(chat_server); B(chat_private); B(chat_poke);
    B(chat_max_visible); B(chat_history); B(chat_retention_s);
    B(chat_timestamp); B(chat_sender); B(chat_channel_name); B(chat_newest_top);
    B(chat_sender_color); B(chat_background);
#undef B

    // --- live facts ------------------------------------------------------------------------
    Live& l = g_live;
    l.ind_names.clear();
    l.notif_names.clear();
    for (int i = 0; i < IndCount; ++i) l.ind_names.push_back(pretty(kIndNames[i]));
    for (int i = 0; i < NotifCount; ++i) l.notif_names.push_back(pretty(kNotifNames[i]));
#define L(field) m.Bind(#field, &l.field)
    L(alive); L(legacy); L(channel_customised); L(conn); L(server); L(channel); L(parent); L(count);
    L(notice); L(key_name); L(channel_id); L(people); L(users); L(channels); L(profiles);
    L(selected); L(new_profile); L(status); L(version); L(ind_names); L(notif_names);
    L(anchor_names); L(sort_names); L(icon_names);
#undef L

    // --- buttons ---------------------------------------------------------------------------
    m.BindEventCallback("close", [&h](Rml::DataModelHandle, Rml::Event&, Args) { h.open = false; });
    m.BindEventCallback("save", [&h](Rml::DataModelHandle, Rml::Event&, Args) { save(h); });
    m.BindEventCallback("reload", [&h](Rml::DataModelHandle, Rml::Event&, Args) { reload(h); });
    m.BindEventCallback("reset", [&h](Rml::DataModelHandle, Rml::Event&, Args) {
        const bool demo = h.cfg.demo;
        h.cfg = Config{};
        h.cfg.demo = demo;
    });
    m.BindEventCallback("select_user", [&h](Rml::DataModelHandle, Rml::Event&, Args a) {
        if (a.empty()) return;
        g_live.selected = a[0].Get<std::string>();
        h.cfg.users[g_live.selected];  // the editor needs an entry to bind to (as the old menu did)
    });
    m.BindEventCallback("remove_user", [&h](Rml::DataModelHandle, Rml::Event&, Args a) {
        if (!a.empty()) h.cfg.users.erase(a[0].Get<std::string>());
        g_live.selected.clear();
    });
    m.BindEventCallback("customise_channel", [&h](Rml::DataModelHandle, Rml::Event&, Args) {
        h.cfg.channels[snapshot(h)->channel_id];
    });
    m.BindEventCallback("remove_channel", [&h](Rml::DataModelHandle, Rml::Event&, Args a) {
        if (!a.empty()) h.cfg.channels.erase(a[0].Get<uint64_t>());
    });
    m.BindEventCallback("profile_load", [&h](Rml::DataModelHandle, Rml::Event&, Args a) {
        const std::string p = a.empty() ? "" : a[0].Get<std::string>();
        if (config::load_profile(h.ini, p)) {
            reload(h);
            g_live.status = "Loaded " + p;
        } else {
            g_live.status = "Could not load " + p;
        }
    });
    m.BindEventCallback("profile_overwrite", [&h](Rml::DataModelHandle, Rml::Event&, Args a) {
        const std::string p = a.empty() ? "" : a[0].Get<std::string>();
        save(h);
        g_live.status = config::save_profile(h.ini, p) ? "Saved " + p : "Could not save " + p;
    });
    m.BindEventCallback("profile_delete", [&h](Rml::DataModelHandle, Rml::Event&, Args a) {
        const std::string p = a.empty() ? "" : a[0].Get<std::string>();
        g_live.status = config::delete_profile(h.ini, p) ? "Deleted " + p : "Could not delete " + p;
    });
    m.BindEventCallback("profile_save_as", [&h](Rml::DataModelHandle, Rml::Event&, Args) {
        const std::string& p = g_live.new_profile;
        if (p.empty()) return;
        save(h);
        g_live.status = config::save_profile(h.ini, p) ? "Saved " + p : "Could not save " + p;
    });
    g_model = m.GetModelHandle();

    refresh(h);
    g_doc = ctx.LoadDocument("menu.rml");
    if (!g_doc) {
        log::error("menu: could not load menu.rml");
        return false;
    }
    return true;
}

void show(bool open) {
    if (!g_doc) return;
    if (open) g_doc->Show();
    else g_doc->Hide();
}

void sync(Host& h) {
    refresh(h);
    g_model.DirtyAllVariables();
    if (g_doc) g_doc->SetClass("dark", h.cfg.menu_dark);  // theme.rcss switches palettes on body.dark
}

}  // namespace yap::menu
