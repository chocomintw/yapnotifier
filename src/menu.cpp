#include "menu.h"

#include <Windows.h>

#include <imgui.h>

#include <cstring>
#include <string>

#include "icons.h"
#include "log.h"
#include "teamspeak.h"
#include "update.h"
#include "version.h"

namespace yap::menu {
namespace {
using namespace ImGui;

bool color_edit(const char* label, Color& c, bool optional = false) {
    float f[4] = {static_cast<float>(c & 0xFF) / 255.f, static_cast<float>((c >> 8) & 0xFF) / 255.f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.f, static_cast<float>(c >> 24) / 255.f};
    bool changed = false;
    if (optional) {
        bool on = c != 0;
        PushID(label);
        if (Checkbox("##on", &on)) {
            c = on ? rgba(255, 255, 255) : 0;
            changed = true;
        }
        PopID();
        SameLine();
        if (!on) {
            TextDisabled("%s", label);
            return changed;
        }
        f[0] = static_cast<float>(c & 0xFF) / 255.f;
        f[1] = static_cast<float>((c >> 8) & 0xFF) / 255.f;
        f[2] = static_cast<float>((c >> 16) & 0xFF) / 255.f;
        f[3] = static_cast<float>(c >> 24) / 255.f;
    }
    if (ColorEdit4(label, f, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
        c = rgba(static_cast<unsigned>(f[0] * 255.f + 0.5f), static_cast<unsigned>(f[1] * 255.f + 0.5f),
                 static_cast<unsigned>(f[2] * 255.f + 0.5f), static_cast<unsigned>(f[3] * 255.f + 0.5f));
        if (optional && c == 0) c = rgba(0, 0, 0, 1);  // keep "set" distinct from "unset"
        changed = true;
    }
    return changed;
}

template <class E, size_t N>
bool enum_combo(const char* label, E& v, const char* const (&names)[N]) {
    int i = static_cast<int>(v);
    if (!Combo(label, &i, names, static_cast<int>(N))) return false;
    v = static_cast<E>(i);
    return true;
}
constexpr const char* kAnchorNames[] = {"Top left",    "Top centre",    "Top right",   "Middle left", "Centre",
                                        "Middle right", "Bottom left",  "Bottom centre", "Bottom right"};
constexpr const char* kSortNames[] = {"Channel order", "Alphabetical", "Speaking first"};
constexpr const char* kIconNames[] = {"None",     "Dot",     "Circle",  "Ring",   "Square",  "Diamond", "Triangle",
                                      "Star",     "Chevron", "Microphone", "Microphone muted", "Speaker", "Speaker muted",
                                      "Moon",     "Record",  "Crown",   "Whisper", "Bars"};

bool icon_combo(const char* label, IconShape& v, Color preview) {
    ImVec2 p = GetCursorScreenPos();
    const float h = GetFrameHeight();
    icons::draw(GetWindowDrawList(), v, p.x + h * 0.5f, p.y + h * 0.5f, h * 0.6f, preview);
    Dummy({h, h});
    SameLine();
    return enum_combo(label, v, kIconNames);
}

bool text_input(const char* label, std::string& s, size_t cap = 256) {
    std::string buf = s;
    buf.resize(cap, '\0');
    if (!InputText(label, buf.data(), cap)) return false;
    s = buf.c_str();
    return true;
}

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

// --- tabs ------------------------------------------------------------------------
void tab_status(Host& h, const ts::Snapshot& snap) {
    Config& c = h.cfg;
    if (!snap.alive)
        TextColored({1.f, 0.6f, 0.4f, 1.f}, "TS3 plugin: not connected (enable the YapNotifier plugin in TeamSpeak)");
    else if (snap.legacy)
        TextColored({1.f, 0.85f, 0.3f, 1.f}, "TS3 plugin is outdated: install YapNotifier.ts3_plugin from the release");
    else
        TextDisabled("TS3 plugin: connected");
    if (auto n = update::notice(); !n->empty()) TextColored({1.f, 0.85f, 0.3f, 1.f}, "%s", n->c_str());
    if (snap.conn == 2) {
        Text("Server: %s", snap.server_name.c_str());
        Text("Channel: %s%s%s (%zu)", snap.parent.c_str(), snap.parent.empty() ? "" : " / ", snap.channel.c_str(),
             snap.users.size());
    } else {
        TextDisabled(snap.conn == 1 ? "Connecting..." : "Not connected to a server");
    }
    Separator();
    Checkbox("Demo mode (fake roster + events, no TeamSpeak needed)", &c.demo);
    InputInt("UDP port", &c.port);
    Checkbox("Auto-update on launch", &c.auto_update);
    TextDisabled("%s toggles this menu (menu_key in the ini)", key_name(c.menu_key).c_str());
}

void tab_layout(Config& c) {
    SeparatorText("Roster block");
    enum_combo("Anchor", c.anchor, kAnchorNames);
    DragFloat("Offset X", &c.pos_x, 1.f, 0.f, 4000.f, "%.0f px");
    DragFloat("Offset Y", &c.pos_y, 1.f, 0.f, 4000.f, "%.0f px");
    Checkbox("Background panel", &c.show_panel);
    if (c.show_panel) {
        color_edit("Panel colour", c.panel_color);
        SliderFloat("Corner radius", &c.corner_radius, 0.f, 32.f, "%.0f");
    }
    SeparatorText("Look");
    SliderFloat("Master opacity", &c.master_opacity, 0.f, 1.f);
    SliderFloat("Scale", &c.scale, 0.25f, 4.f);
    SliderFloat("Icon size", &c.icon_size, 2.f, 96.f, "%.0f");
    SliderFloat("Row spacing", &c.row_spacing, 0.f, 64.f, "%.0f");
    Checkbox("Text shadow", &c.text_shadow);
    SameLine();
    Checkbox("Text outline", &c.text_outline);
    color_edit("Text colour", c.text_color);
    color_edit("Secondary text", c.text_secondary);
    SeparatorText("Font (applies on next launch)");
    text_input("Font file (.ttf, relative to plugins dir; empty = Quicksand)", c.font_file);
    SliderFloat("Font size", &c.font_size, 6.f, 96.f, "%.0f px");
    SeparatorText("Idle");
    Checkbox("Fade when idle", &c.fade_when_idle);
    if (c.fade_when_idle) {
        SliderInt("Idle after (ms)", &c.idle_after_ms, 1000, 120000);
        SliderFloat("Idle opacity", &c.idle_opacity, 0.f, 1.f);
    }
}

void tab_roster(Config& c) {
    SeparatorText("Title");
    Checkbox("Show channel title", &c.show_title);
    Checkbox("Parent channel", &c.show_parent);
    SameLine();
    Checkbox("User count", &c.show_count);
    SameLine();
    Checkbox("Server name", &c.show_server);
    text_input("Title format ({channel} {parent} {server} {count})", c.title_format);
    text_input("Disconnected text", c.disconnected_text);
    color_edit("Title colour", c.title_color);
    Checkbox("Draw while disconnected", &c.show_when_disconnected);
    SeparatorText("List");
    Checkbox("Stealth: only show who is talking", &c.only_show_talking);
    Checkbox("Show muted users", &c.show_muted_users);
    Checkbox("Show yourself", &c.show_local_user);
    SameLine();
    Checkbox("Highlight yourself", &c.highlight_local_user);
    color_edit("Your colour", c.local_user_color);
    enum_combo("Sort", c.sort, kSortNames);
    SliderInt("Max visible users", &c.max_visible_users, 1, 64);
    Checkbox("Show '+N more'", &c.show_overflow_count);
    SliderFloat("Max name width", &c.max_name_width, 20.f, 800.f, "%.0f px");
    SeparatorText("Friends (from the TeamSpeak contact list)");
    Checkbox("Colour friends", &c.color_friends);
    color_edit("Friend colour", c.friend_color);
    Checkbox("Show contact nickname tag", &c.show_friend_tag);
    color_edit("Tag colour", c.friend_tag_color);
    SeparatorText("Speaking animation");
    SliderInt("Attack (ms)", &c.speaking_attack_ms, 0, 1000);
    SliderInt("Release (ms)", &c.speaking_release_ms, 0, 2000);
    Checkbox("Pulse speaking icon", &c.pulse);
    if (c.pulse) SliderFloat("Pulse Hz", &c.pulse_hz, 0.1f, 10.f);
}

void tab_indicators(Config& c) {
    TextDisabled("Commander / priority speaker icons lead the name; the rest trail it.");
    for (int i = 0; i < IndCount; ++i) {
        Indicator& ind = c.ind[i];
        PushID(i);
        std::string label = kIndNames[i];
        for (char& ch : label) ch = ch == '_' ? ' ' : ch;
        if (CollapsingHeader(label.c_str())) {
            Checkbox("Enabled", &ind.enabled);
            icon_combo("Icon", ind.icon, ind.icon_color);
            color_edit("Icon colour", ind.icon_color);
            color_edit("Name colour", ind.text_color, true);
            SliderFloat("Dim row", &ind.dim, 0.f, 1.f);
        }
        PopID();
    }
}

void tab_notifications(Config& c) {
    Checkbox("Enabled", &c.notif_enabled);
    enum_combo("Anchor", c.notif_anchor, kAnchorNames);
    DragFloat("Offset X", &c.notif_x, 1.f, 0.f, 4000.f, "%.0f px");
    DragFloat("Offset Y", &c.notif_y, 1.f, 0.f, 4000.f, "%.0f px");
    SliderFloat("Width", &c.notif_width, 80.f, 1200.f, "%.0f px");
    SliderInt("Max visible", &c.notif_max_visible, 1, 20);
    Checkbox("Stack upwards", &c.notif_stack_up);
    SameLine();
    Checkbox("Merge duplicates", &c.notif_merge_duplicates);
    SameLine();
    Checkbox("Accent bar", &c.notif_accent_bar);
    SliderInt("Fade in (ms)", &c.notif_in_ms, 0, 2000);
    SliderInt("Fade out (ms)", &c.notif_out_ms, 0, 2000);
    SliderInt("Quiet after connect (ms)", &c.notif_suppress_after_connect_ms, 0, 20000);
    color_edit("Background", c.notif_background);
    SeparatorText("Categories");
    for (int i = 0; i < NotifCount; ++i) {
        NotifCategory& n = c.notif[i];
        PushID(i);
        std::string label = kNotifNames[i];
        for (char& ch : label) ch = ch == '_' ? ' ' : ch;
        if (CollapsingHeader(label.c_str())) {
            Checkbox("Enabled", &n.enabled);
            text_input("Format", n.format);
            color_edit("Colour", n.color);
            SliderInt("Hold (ms)", &n.hold_ms, 0, 30000);
        }
        PopID();
    }
    TextDisabled("Placeholders: {name} {channel} {from} {to} {previous} {count} {status} {server} {message}");
}

void tab_chat(Config& c) {
    Checkbox("Show chat feed", &c.chat_enabled);
    enum_combo("Anchor", c.chat_anchor, kAnchorNames);
    DragFloat("Offset X", &c.chat_x, 1.f, 0.f, 4000.f, "%.0f px");
    DragFloat("Offset Y", &c.chat_y, 1.f, 0.f, 4000.f, "%.0f px");
    SliderFloat("Width", &c.chat_width, 80.f, 1600.f, "%.0f px");
    SeparatorText("Categories");
    Checkbox("Channel", &c.chat_channel);
    SameLine();
    Checkbox("Server", &c.chat_server);
    SameLine();
    Checkbox("Private", &c.chat_private);
    SameLine();
    Checkbox("Pokes", &c.chat_poke);
    SeparatorText("Lines");
    SliderInt("Visible lines", &c.chat_max_visible, 1, 30);
    SliderInt("History", &c.chat_history, 1, 500);
    SliderInt("Retention (s, 0 = forever)", &c.chat_retention_s, 0, 3600);
    Checkbox("Timestamp", &c.chat_timestamp);
    SameLine();
    Checkbox("Sender", &c.chat_sender);
    SameLine();
    Checkbox("Channel name", &c.chat_channel_name);
    SameLine();
    Checkbox("Newest on top", &c.chat_newest_top);
    color_edit("Sender colour", c.chat_sender_color);
    color_edit("Background", c.chat_background);
}

void user_editor(Config& c, const std::string& uid) {
    UserOverride& u = c.users[uid];
    color_edit("Name colour", u.name_color, true);
    color_edit("Speaking colour", u.speaking_color, true);
    icon_combo("Icon", u.icon, u.icon_color);
    color_edit("Icon colour", u.icon_color);
    text_input("Display name", u.display_name, 64);
    const char* friend_opts[] = {"As TeamSpeak says", "Friend", "Not a friend"};
    int fi = u.is_friend < 0 ? 0 : u.is_friend ? 1 : 2;
    if (Combo("Friend", &fi, friend_opts, 3)) u.is_friend = fi == 0 ? -1 : fi == 1 ? 1 : 0;
    text_input("Friend tag", u.friend_tag, 32);
    if (Button("Remove override")) c.users.erase(uid);
}

void tab_users(Config& c, const ts::Snapshot& snap) {
    static std::string selected;
    TextDisabled("People in your channel");
    for (const auto& u : snap.users) {
        PushID(u.uid.c_str());
        const bool has = c.users.count(u.uid) != 0;
        if (Selectable(u.nickname.c_str(), selected == u.uid)) selected = u.uid;
        if (has) {
            SameLine();
            TextDisabled("(customised)");
        }
        PopID();
    }
    if (snap.users.empty()) TextDisabled("(nobody: connect to TeamSpeak or enable demo mode)");
    std::vector<std::string> others;
    for (const auto& [uid, u] : c.users) {
        bool present = false;
        for (const auto& su : snap.users) present |= su.uid == uid;
        if (!present) others.push_back(uid);
    }
    if (!others.empty()) {
        TextDisabled("Other customised users");
        for (const auto& uid : others) {
            const auto& u = c.users[uid];
            std::string label = u.display_name.empty() ? uid : u.display_name + " (" + uid + ")";
            if (Selectable(label.c_str(), selected == uid)) selected = uid;
        }
    }
    if (!selected.empty()) {
        Separator();
        Text("Customise %s", selected.c_str());
        user_editor(c, selected);
        if (!c.users.count(selected)) selected.clear();
    }
}

void tab_channel(Config& c, const ts::Snapshot& snap) {
    if (snap.conn != 2) {
        TextDisabled("Join a channel to customise it.");
    } else {
        Text("Current channel: %s (id %llu)", snap.channel.c_str(), static_cast<unsigned long long>(snap.channel_id));
        if (!c.channels.count(snap.channel_id) && Button("Customise this channel")) c.channels[snap.channel_id] = {};
    }
    for (auto it = c.channels.begin(); it != c.channels.end();) {
        PushID(static_cast<int>(it->first));
        std::string label = "Channel " + std::to_string(it->first) + (it->first == snap.channel_id ? " (current)" : "");
        bool erase = false;
        if (CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ChannelOverride& ch = it->second;
            color_edit("Title colour", ch.title_color, true);
            text_input("Display name", ch.display_name, 64);
            icon_combo("Icon", ch.icon, ch.icon_color);
            color_edit("Icon colour", ch.icon_color);
            bool has_op = ch.opacity >= 0.f;
            if (Checkbox("Override opacity", &has_op)) ch.opacity = has_op ? 1.f : -1.f;
            if (has_op) SliderFloat("Opacity", &ch.opacity, 0.f, 1.f);
            erase = Button("Remove override");
        }
        PopID();
        it = erase ? c.channels.erase(it) : std::next(it);
    }
}

void tab_profiles(Host& h) {
    static std::string name;
    static std::string status;
    TextDisabled("A profile is a saved copy of the current settings.");
    for (const auto& p : config::list_profiles(h.ini)) {
        PushID(p.c_str());
        Text("%s", p.c_str());
        SameLine();
        if (SmallButton("Load")) {
            if (config::load_profile(h.ini, p)) {
                reload(h);
                status = "Loaded " + p;
            } else {
                status = "Could not load " + p;
            }
        }
        SameLine();
        if (SmallButton("Overwrite")) {
            save(h);
            status = config::save_profile(h.ini, p) ? "Saved " + p : "Could not save " + p;
        }
        SameLine();
        if (SmallButton("Delete")) status = config::delete_profile(h.ini, p) ? "Deleted " + p : "Could not delete " + p;
        PopID();
    }
    Separator();
    text_input("New profile name", name, 64);
    SameLine();
    if (Button("Save as") && !name.empty()) {
        save(h);
        status = config::save_profile(h.ini, name) ? "Saved " + name : "Could not save " + name;
    }
    if (!status.empty()) TextDisabled("%s", status.c_str());
}
}  // namespace

void draw(Host& h) {
    SetNextWindowSize({560, 0}, ImGuiCond_FirstUseEver);
    SetNextWindowPos({h.cfg.pos_x + 40.f, h.cfg.pos_y + 120.f}, ImGuiCond_FirstUseEver);
    Begin("YapNotifier v" YAP_VERSION, &h.open, ImGuiWindowFlags_AlwaysAutoResize);
    auto snap = h.cfg.demo ? std::shared_ptr<const ts::Snapshot>(&hud::demo_snapshot(), [](const ts::Snapshot*) {})
                           : ts::snapshot();
    if (BeginTabBar("tabs")) {
        if (BeginTabItem("Status")) { tab_status(h, *snap); EndTabItem(); }
        if (BeginTabItem("Layout")) { tab_layout(h.cfg); EndTabItem(); }
        if (BeginTabItem("Roster")) { tab_roster(h.cfg); EndTabItem(); }
        if (BeginTabItem("Indicators")) { tab_indicators(h.cfg); EndTabItem(); }
        if (BeginTabItem("Notifications")) { tab_notifications(h.cfg); EndTabItem(); }
        if (BeginTabItem("Chat")) { tab_chat(h.cfg); EndTabItem(); }
        if (BeginTabItem("Users")) { tab_users(h.cfg, *snap); EndTabItem(); }
        if (BeginTabItem("Channel")) { tab_channel(h.cfg, *snap); EndTabItem(); }
        if (BeginTabItem("Profiles")) { tab_profiles(h); EndTabItem(); }
        EndTabBar();
    }
    Separator();
    if (Button("Save")) save(h);
    SameLine();
    if (Button("Reload from disk")) reload(h);
    SameLine();
    if (Button("Reset to defaults")) {
        const bool demo = h.cfg.demo;
        h.cfg = Config{};
        h.cfg.demo = demo;
    }
    SameLine();
    TextDisabled("edits apply live; Save writes YapNotifier.ini");
    End();
}

}  // namespace yap::menu
