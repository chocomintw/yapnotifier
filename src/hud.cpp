#include "hud.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

#include "icons.h"
#include "log.h"
#include "yap_protocol.h"

namespace yap::hud {
namespace {
namespace proto = yap::proto;
constexpr float kPi = 3.14159265358979323846f;

// What hud.rml binds to. Colours are "#RRGGBBAA", sizes "<n>dp" so the RCSS can use them verbatim.
struct IconView {
    std::string src, color;
    float opacity = 1.f;
};
struct RowView {
    std::string name, name_color, tag, tag_color;
    float opacity = 1.f;
    std::vector<IconView> leading, trailing;
};
struct ToastView {
    std::string prefix, text, color, pill_text;  // pill = category colour with ink/white text
    float opacity = 1.f;
};
struct ChatView {
    std::string prefix, body;
};
struct View {
    // config-derived look
    std::string font_size = "16dp", icon_size = "10dp", row_gap = "2dp", max_name = "180dp";
    std::string text_color = "#FFFFFFFF", text_secondary = "#FFFFFFFF";
    bool shadow = false, outline = false;
    // roster block
    bool show_roster = false, panel = false, legacy = false;
    float roster_opacity = 1.f;
    std::string panel_color = "transparent", radius = "0dp", title, title_color = "#FFFFFFFF", title_icon;
    std::string title_icon_color = "#FFFFFFFF", more;
    std::vector<RowView> rows;
    // toasts (DESIGN.md card: theme surface + hairline + ink, category as a pastel pill)
    bool accent_bar = false;
    std::string notif_width = "280dp", notif_bg = "#FFFFFFFF", notif_border = "#E6E5E0FF", notif_text = "#26251EFF";
    std::vector<ToastView> toasts;
    // chat (same card as the toasts)
    std::string chat_width = "420dp", chat_bg = "#FFFFFFFF", chat_border = "#E6E5E0FF", chat_text = "#26251EFF";
    std::string chat_sender_color = "#FFFFFFFF";
    std::vector<ChatView> chat;
    std::string notice;
};

View g_view;
Rml::DataModelHandle g_model;
Rml::ElementDocument* g_doc = nullptr;

std::string dp(float v) { return std::to_string(static_cast<int>(std::lround(v))) + "dp"; }
std::string hex(Color c, float alpha = 1.f) { return config::format_color(icons::with_alpha(c, alpha)); }

// Sets a property only when its value changed, so per-frame syncs never dirty layout.
void set_prop(Rml::Element* el, const char* name, const std::string& value) {
    const Rml::Property* cur = el->GetLocalProperty(name);
    if (cur && cur->ToString() == value) return;
    el->SetProperty(name, value);
}

// 9-point anchoring: pin the edge(s) the anchor names; centre rows/columns sit at 50%
// and are pulled back by half their own size (same placement as the pre-RmlUi HUD).
void place(Rml::Element* el, Anchor a, float x, float y) {
    const int col = static_cast<int>(a) % 3, row = static_cast<int>(a) / 3;
    // Never off-screen: clamp the offset against the block's last laid-out size and the
    // context (= game client area), all in dp.
    if (Rml::Context* ctx = el->GetContext()) {
        const float ratio = ctx->GetDensityIndependentPixelRatio();
        const Rml::Vector2f size = el->GetBox().GetSize(Rml::BoxArea::Border) / ratio;
        const Rml::Vector2f extent = Rml::Vector2f(ctx->GetDimensions()) / ratio;
        x = roster::clamp_offset(x, size.x, extent.x, col == 1);
        y = roster::clamp_offset(y, size.y, extent.y, row == 1);
    }
    set_prop(el, "left", col == 0 ? dp(x) : col == 1 ? "50%" : "auto");
    set_prop(el, "right", col == 2 ? dp(x) : "auto");
    set_prop(el, "top", row == 0 ? dp(y) : row == 1 ? "50%" : "auto");
    set_prop(el, "bottom", row == 2 ? dp(y) : "auto");
    set_prop(el, "margin-left", col == 1 ? dp(x) : "0px");
    set_prop(el, "margin-top", row == 1 ? dp(y) : "0px");
    std::string tf = "none";
    if (col == 1 && row == 1) tf = "translate(-50%, -50%)";
    else if (col == 1) tf = "translateX(-50%)";
    else if (row == 1) tf = "translateY(-50%)";
    set_prop(el, "transform", tf);
    set_prop(el, "text-align", col == 0 ? "left" : col == 1 ? "center" : "right");
}

std::string title_text(const Config& cfg, const ts::Snapshot& snap, const ChannelOverride* ov) {
    const std::string channel = ov && !ov->display_name.empty() ? ov->display_name : snap.channel;
    std::string t = notify::format_template(cfg.title_format, {{"channel", channel},
                                                              {"parent", snap.parent},
                                                              {"server", snap.server_name},
                                                              {"count", std::to_string(snap.users.size())}});
    if (cfg.show_parent && !snap.parent.empty() && cfg.title_format.find("{parent}") == std::string::npos)
        t = snap.parent + " / " + t;
    if (cfg.show_server && !snap.server_name.empty() && cfg.title_format.find("{server}") == std::string::npos)
        t = snap.server_name + " - " + t;
    if (cfg.show_count && cfg.title_format.find("{count}") == std::string::npos)
        t += " (" + std::to_string(snap.users.size()) + ")";
    return t;
}

// --- roster block -----------------------------------------------------------------
void sync_roster(View& v, State& st, const Config& cfg, const ts::Snapshot& snap, float master, double t) {
    const bool live = snap.alive && snap.conn == 2;
    v.show_roster = live || cfg.show_when_disconnected;
    v.rows.clear();
    v.title.clear();
    v.title_icon.clear();
    v.more.clear();
    v.legacy = false;
    if (!v.show_roster) return;

    const ChannelOverride* chov = nullptr;
    if (auto it = cfg.channels.find(snap.channel_id); live && it != cfg.channels.end()) chov = &it->second;
    v.roster_opacity = master * (chov && chov->opacity >= 0.f ? chov->opacity : 1.f) * (live ? 1.f : 0.8f);

    Color title_color = cfg.title_color;
    if (live && cfg.show_title) {
        v.title = title_text(cfg, snap, chov);
        if (chov && chov->title_color) title_color = chov->title_color;
        if (chov && chov->icon != IconShape::None) v.title_icon = icons::svg(chov->icon);
        v.title_icon_color = hex(chov ? chov->icon_color : cfg.title_color);
    } else if (!live) {
        v.title = cfg.disconnected_text;
        title_color = cfg.text_secondary;
    }
    v.title_color = hex(title_color);
    if (!live) return;

    auto users = roster::visible(snap, cfg);
    int overflow = 0;
    if (users.size() > static_cast<size_t>(cfg.max_visible_users)) {
        overflow = static_cast<int>(users.size()) - cfg.max_visible_users;
        users.resize(static_cast<size_t>(cfg.max_visible_users));
    }
    for (const ts::User* u : users) {
        roster::Resolved r = roster::resolve(*u, cfg, st.env.level(u->uid));
        RowView row;
        row.name = r.name;
        row.name_color = hex(r.name_color);
        row.tag = r.tag;
        row.tag_color = hex(r.tag_color);
        row.opacity = r.opacity;
        for (const auto& ic : r.leading) row.leading.push_back({icons::svg(ic.shape), hex(ic.color), 1.f});
        for (const auto& ic : r.trailing) {
            float ia = 1.f;
            if (ic.shape == cfg.ind[IndSpeaking].icon && r.level > 0.f) {
                ia = r.level;
                if (cfg.pulse) ia *= 1.f - 0.35f * (0.5f + 0.5f * std::sin(static_cast<float>(t) * 2.f * kPi * cfg.pulse_hz));
            }
            row.trailing.push_back({icons::svg(ic.shape), hex(ic.color), ia});
        }
        v.rows.push_back(std::move(row));
    }
    if (overflow > 0 && cfg.show_overflow_count) v.more = "+" + std::to_string(overflow) + " more";
    v.legacy = snap.legacy;
}

// --- toasts ---------------------------------------------------------------------------
// Perceived luminance 0..1, for picking ink or white text over a colour.
float luminance(Color c) {
    return (0.299f * (c & 0xff) + 0.587f * ((c >> 8) & 0xff) + 0.114f * ((c >> 16) & 0xff)) / 255.f;
}

// DESIGN.md card tokens: light = card / hairline / ink, dark = its ink inversion. The theme
// picks the card; a custom `custom` colour wins, and text/hairline then follow whatever the
// card actually is (a user's black stays readable in the light theme).
void card_colors(const Config& cfg, Color custom, std::string& bg, std::string& border, std::string& text) {
    const Color c = custom ? custom : cfg.dark_theme ? rgba(0x26, 0x25, 0x1e) : rgba(0xff, 0xff, 0xff);
    const bool dark = luminance(c) < 0.5f;
    bg = hex(c);
    border = hex(dark ? rgba(0x3a, 0x38, 0x30) : rgba(0xe6, 0xe5, 0xe0));
    text = hex(dark ? rgba(0xf7, 0xf7, 0xf4) : rgba(0x26, 0x25, 0x1e));
}

void sync_toasts(View& v, State& st, const Config& cfg, float master) {
    card_colors(cfg, cfg.notif_background, v.notif_bg, v.notif_border, v.notif_text);
    v.toasts.clear();
    if (!cfg.notif_enabled) return;
    for (const auto& t : st.toasts.items()) {
        const Color col = cfg.notif[t.cat].color;
        ToastView b;
        b.prefix = kNotifPrefixes[t.cat];
        b.text = t.text + (t.count > 1 ? " x" + std::to_string(t.count) : "");
        b.color = hex(col);
        // ink on the pastels, white on the dark ones (gold, error)
        b.pill_text = hex(luminance(col) > 0.55f ? rgba(0x26, 0x25, 0x1e) : rgba(0xff, 0xff, 0xff));
        b.opacity = notify::Queue::alpha(t, cfg) * master;
        v.toasts.push_back(std::move(b));
    }
    // Items are oldest first; stacking down shows the oldest on top, stacking up flips it.
    if (cfg.notif_stack_up) std::reverse(v.toasts.begin(), v.toasts.end());
}

// --- chat feed -------------------------------------------------------------------
void sync_chat(View& v, State& st, const Config& cfg, uint64_t now_ms) {
    v.chat.clear();
    if (!cfg.chat_enabled) return;
    std::vector<const notify::ChatLine*> lines;
    for (auto it = st.chat.rbegin(); it != st.chat.rend() && lines.size() < static_cast<size_t>(cfg.chat_max_visible); ++it)
        if (notify::chat_visible(*it, cfg, now_ms)) lines.push_back(&*it);
    if (!cfg.chat_newest_top) std::reverse(lines.begin(), lines.end());
    for (const auto* l : lines) {
        ChatView x;
        char ts[8] = {};
        if (cfg.chat_timestamp) std::snprintf(ts, sizeof ts, "%02d:%02d ", l->hour, l->minute);
        x.prefix = ts;
        x.prefix += l->cat == notify::ChatCat::Channel ? "# " : l->cat == notify::ChatCat::Private ? "@ " : "! ";
        if (cfg.chat_channel_name && !l->channel.empty()) x.prefix += "[" + l->channel + "] ";
        if (cfg.chat_sender) x.prefix += l->sender + ": ";
        x.body = l->text;
        v.chat.push_back(std::move(x));
    }
}
}  // namespace

void feed(State& st, const Config& cfg, const std::vector<ts::Event>& events, uint64_t now_ms) {
    for (const auto& e : events) {
        if (e.kind == ts::Event::Conn && e.f[1] == "connected") st.toasts.mark_connected();
        Notif cat;
        std::string text;
        if (notify::toast_for(e, cfg, cat, text)) st.toasts.push(cat, std::move(text), cfg);
        notify::ChatLine line;
        if (notify::chat_line_for(e, line)) {
            line.at_ms = now_ms;
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            line.hour = tm.tm_hour;
            line.minute = tm.tm_min;
            st.chat.push_back(std::move(line));
            while (st.chat.size() > static_cast<size_t>(cfg.chat_history)) st.chat.erase(st.chat.begin());
        }
        st.idle_ms = 0.f;
    }
}

bool init(Rml::Context& ctx) {
    Rml::DataModelConstructor m = ctx.CreateDataModel("hud");
    if (!m) {
        log::error("hud: could not create the data model");
        return false;
    }
    if (auto h = m.RegisterStruct<IconView>()) {
        h.RegisterMember("src", &IconView::src);
        h.RegisterMember("color", &IconView::color);
        h.RegisterMember("opacity", &IconView::opacity);
    }
    m.RegisterArray<std::vector<IconView>>();
    if (auto h = m.RegisterStruct<RowView>()) {
        h.RegisterMember("name", &RowView::name);
        h.RegisterMember("name_color", &RowView::name_color);
        h.RegisterMember("tag", &RowView::tag);
        h.RegisterMember("tag_color", &RowView::tag_color);
        h.RegisterMember("opacity", &RowView::opacity);
        h.RegisterMember("leading", &RowView::leading);
        h.RegisterMember("trailing", &RowView::trailing);
    }
    m.RegisterArray<std::vector<RowView>>();
    if (auto h = m.RegisterStruct<ToastView>()) {
        h.RegisterMember("prefix", &ToastView::prefix);
        h.RegisterMember("text", &ToastView::text);
        h.RegisterMember("color", &ToastView::color);
        h.RegisterMember("pill_text", &ToastView::pill_text);
        h.RegisterMember("opacity", &ToastView::opacity);
    }
    m.RegisterArray<std::vector<ToastView>>();
    if (auto h = m.RegisterStruct<ChatView>()) {
        h.RegisterMember("prefix", &ChatView::prefix);
        h.RegisterMember("body", &ChatView::body);
    }
    m.RegisterArray<std::vector<ChatView>>();

    View& v = g_view;
    m.Bind("font_size", &v.font_size);
    m.Bind("icon_size", &v.icon_size);
    m.Bind("row_gap", &v.row_gap);
    m.Bind("max_name", &v.max_name);
    m.Bind("text_color", &v.text_color);
    m.Bind("text_secondary", &v.text_secondary);
    m.Bind("shadow", &v.shadow);
    m.Bind("outline", &v.outline);
    m.Bind("show_roster", &v.show_roster);
    m.Bind("panel", &v.panel);
    m.Bind("legacy", &v.legacy);
    m.Bind("roster_opacity", &v.roster_opacity);
    m.Bind("panel_color", &v.panel_color);
    m.Bind("radius", &v.radius);
    m.Bind("title", &v.title);
    m.Bind("title_color", &v.title_color);
    m.Bind("title_icon", &v.title_icon);
    m.Bind("title_icon_color", &v.title_icon_color);
    m.Bind("more", &v.more);
    m.Bind("rows", &v.rows);
    m.Bind("accent_bar", &v.accent_bar);
    m.Bind("notif_width", &v.notif_width);
    m.Bind("notif_bg", &v.notif_bg);
    m.Bind("notif_border", &v.notif_border);
    m.Bind("notif_text", &v.notif_text);
    m.Bind("toasts", &v.toasts);
    m.Bind("chat_width", &v.chat_width);
    m.Bind("chat_bg", &v.chat_bg);
    m.Bind("chat_border", &v.chat_border);
    m.Bind("chat_text", &v.chat_text);
    m.Bind("chat_sender_color", &v.chat_sender_color);
    m.Bind("chat", &v.chat);
    m.Bind("notice", &v.notice);
    g_model = m.GetModelHandle();

    g_doc = ctx.LoadDocument("hud.rml");
    if (!g_doc) {
        log::error("hud: could not load hud.rml");
        return false;
    }
    g_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    return true;
}

void sync(State& st, const Config& cfg, const ts::Snapshot& snap, float dt_ms, uint64_t now_ms, const std::string& notice) {
    st.env.tick(snap.users, dt_ms, cfg);
    st.toasts.tick(dt_ms, cfg);

    bool busy = st.env.any_active() || !st.toasts.items().empty();
    st.idle_ms = busy ? 0.f : st.idle_ms + dt_ms;
    const float target = cfg.fade_when_idle && st.idle_ms > static_cast<float>(cfg.idle_after_ms) ? 1.f : 0.f;
    st.idle_fade += (target - st.idle_fade) * std::min(1.f, dt_ms / 400.f);
    const float master = cfg.master_opacity * (1.f - st.idle_fade * (1.f - cfg.idle_opacity));

    View& v = g_view;
    v.font_size = dp(cfg.font_size);
    v.icon_size = dp(cfg.icon_size);
    v.row_gap = dp(cfg.row_spacing);
    v.max_name = dp(cfg.max_name_width);
    v.text_color = hex(cfg.text_color);
    v.text_secondary = hex(cfg.text_secondary);
    v.shadow = cfg.text_shadow && !cfg.text_outline;
    v.outline = cfg.text_outline;
    v.panel = cfg.show_panel;
    v.panel_color = cfg.show_panel ? hex(cfg.panel_color) : "transparent";
    v.radius = dp(cfg.corner_radius);
    v.accent_bar = cfg.notif_accent_bar;
    v.notif_width = dp(cfg.notif_width);
    v.chat_width = dp(cfg.chat_width);
    card_colors(cfg, cfg.chat_background, v.chat_bg, v.chat_border, v.chat_text);  // master goes on #chat's opacity
    v.chat_sender_color = hex(cfg.chat_sender_color);
    v.notice = notice;
    sync_roster(v, st, cfg, snap, master, static_cast<double>(now_ms) / 1000.0);
    sync_toasts(v, st, cfg, master);
    sync_chat(v, st, cfg, now_ms);

    place(g_doc->GetElementById("roster"), cfg.anchor, cfg.pos_x, cfg.pos_y);
    place(g_doc->GetElementById("toasts"), cfg.notif_anchor, cfg.notif_x, cfg.notif_y);
    place(g_doc->GetElementById("chat"), cfg.chat_anchor, cfg.chat_x, cfg.chat_y);
    set_prop(g_doc->GetElementById("chat"), "opacity", std::to_string(master));
    g_model.DirtyAllVariables();
}

// --- demo --------------------------------------------------------------------------
const ts::Snapshot& demo_snapshot() {
    static const ts::Snapshot snap = [] {
        ts::Snapshot s;
        s.alive = true;
        s.conn = 2;
        s.server_name = "Demo Server";
        s.channel_id = 1;
        s.channel = "General";
        s.parent = "Lobby";
        auto add = [&](uint16_t id, uint32_t flags, const char* nick, const char* contact = "") {
            s.users.push_back({id, flags, "demo-" + std::to_string(id), contact, nick});
        };
        add(1, proto::Self, "You");
        add(2, proto::Talking, "Alice");
        add(3, proto::Commander, "Bob");
        add(4, proto::InputMuted, "Carol");
        add(5, proto::OutputMuted, "Dave");
        add(6, proto::Away, "Erin");
        add(7, proto::Talking | proto::Whisper, "Frank");
        add(8, proto::Recording, "Grace");
        add(9, proto::Friend, "Friend User", "Bestie");
        add(10, proto::Priority, "A very long nickname that keeps going and going");
        add(11, proto::LocallyMuted, "Heidi");
        return s;
    }();
    return snap;
}

void demo_tick(State& st, const Config& cfg, uint64_t now_ms) {
    if (now_ms < st.demo_next_ms) return;
    st.demo_next_ms = now_ms + 3000;
    using E = ts::Event;
    static const E script[] = {
        {E::Join, {"Lobby", "Alice"}},
        {E::Leave, {"AFK", "Bob"}},
        {E::Switch, {"6", "Lobby", "General"}},
        {E::Conn, {"Demo Server", "connected"}},
        {E::Whisper, {"Ops", "Frank"}},
        {E::Chat, {"channel", "Alice", "General", "anyone up for a round?"}},
        {E::Chat, {"private", "Grace", "", "psst, this is a private message"}},
        {E::Chat, {"poke", "Bob", "", "wake up!"}},
        {E::Chat, {"server", "Server", "", "welcome to the demo"}},
    };
    const E& e = script[static_cast<size_t>(st.demo_step) % std::size(script)];
    ++st.demo_step;
    Config demo_cfg = cfg;
    demo_cfg.chat_channel = demo_cfg.chat_server = demo_cfg.chat_private = demo_cfg.chat_poke = true;
    feed(st, demo_cfg, {e}, now_ms);
}

}  // namespace yap::hud
