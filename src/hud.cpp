#include "hud.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

#include "icons.h"
#include "yap_protocol.h"

namespace yap::hud {
namespace {
namespace proto = yap::proto;
constexpr float kPi = 3.14159265358979323846f;

struct Ctx {
    ImDrawList* dl;
    ImFont* font;
    float fs;       // scaled font size
    float scale;
    float master;   // master opacity incl. idle fade
    ImVec2 display;
    const Config& cfg;
    double t;       // seconds, for the pulse
};

Color alpha(Color c, float a) { return icons::with_alpha(c, a); }

ImVec2 text_size(const Ctx& c, const std::string& s, float size = 0.f, float wrap = 0.f) {
    return c.font->CalcTextSizeA(size > 0.f ? size : c.fs, FLT_MAX, wrap, s.c_str());
}

void text(const Ctx& c, ImVec2 pos, Color color, const std::string& s, float size = 0.f, float wrap = 0.f) {
    if (s.empty()) return;
    const float sz = size > 0.f ? size : c.fs;
    const float a = static_cast<float>(color >> 24) / 255.f;
    if (c.cfg.text_outline) {
        const Color oc = alpha(rgba(0, 0, 0, 230), a);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy)
                    c.dl->AddText(c.font, sz, {pos.x + static_cast<float>(dx), pos.y + static_cast<float>(dy)}, oc,
                                  s.c_str(), nullptr, wrap);
    } else if (c.cfg.text_shadow) {
        c.dl->AddText(c.font, sz, {pos.x + 1.f, pos.y + 1.f}, alpha(rgba(0, 0, 0, 190), a), s.c_str(), nullptr, wrap);
    }
    c.dl->AddText(c.font, sz, pos, color, s.c_str(), nullptr, wrap);
}

// Top-left of a w x h block placed `x, y` px inwards from the anchored edge.
ImVec2 anchor_pos(Anchor a, float x, float y, float w, float h, ImVec2 display) {
    const int col = static_cast<int>(a) % 3, row = static_cast<int>(a) / 3;
    float px = col == 0 ? x : col == 1 ? (display.x - w) * 0.5f + x : display.x - w - x;
    float py = row == 0 ? y : row == 1 ? (display.y - h) * 0.5f + y : display.y - h - y;
    px = std::clamp(px, 0.f, std::max(0.f, display.x - w));
    py = std::clamp(py, 0.f, std::max(0.f, display.y - h));
    return {px, py};
}

float align_of(Anchor a) {
    const int col = static_cast<int>(a) % 3;
    return col == 0 ? 0.f : col == 1 ? 0.5f : 1.f;
}

std::string ellipsize(const Ctx& c, std::string s, float max_w) {
    if (max_w <= 0.f || text_size(c, s).x <= max_w) return s;
    while (!s.empty() && text_size(c, s + "...").x > max_w) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    }
    return s + "...";
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
struct Row {
    roster::Resolved r;
    std::string name;  // ellipsized
    float w = 0.f;
};

void draw_roster(const Ctx& c, State& st, const ts::Snapshot& snap) {
    const Config& cfg = c.cfg;
    const bool live = snap.alive && snap.conn == 2;
    if (!live && !cfg.show_when_disconnected) return;

    const ChannelOverride* chov = nullptr;
    if (auto it = cfg.channels.find(snap.channel_id); live && it != cfg.channels.end()) chov = &it->second;
    const float block_alpha = c.master * (chov && chov->opacity >= 0.f ? chov->opacity : 1.f) * (live ? 1.f : 0.8f);

    const float icon = cfg.icon_size * c.scale;
    const float gap = 4.f * c.scale;
    const float row_h = c.fs + cfg.row_spacing * c.scale;
    const float title_fs = c.fs * 1.15f;
    const float pad = cfg.show_panel ? 8.f * c.scale : 0.f;

    // measure
    std::string title;
    Color title_color = cfg.title_color;
    if (live && cfg.show_title) {
        title = title_text(cfg, snap, chov);
        if (chov && chov->title_color) title_color = chov->title_color;
    } else if (!live) {
        title = cfg.disconnected_text;
        title_color = cfg.text_secondary;
    }
    float title_w = title.empty() ? 0.f : text_size(c, title, title_fs).x;
    if (chov && chov->icon != IconShape::None && !title.empty()) title_w += icon + gap;

    std::vector<Row> rows;
    int overflow = 0;
    std::string legacy_line;
    if (live) {
        auto users = roster::visible(snap, cfg);
        if (users.size() > static_cast<size_t>(cfg.max_visible_users)) {
            overflow = static_cast<int>(users.size()) - cfg.max_visible_users;
            users.resize(static_cast<size_t>(cfg.max_visible_users));
        }
        for (const ts::User* u : users) {
            Row row;
            row.r = roster::resolve(*u, cfg, st.env.level(u->uid));
            row.name = ellipsize(c, row.r.name, cfg.max_name_width * c.scale);
            row.w = text_size(c, row.name).x;
            if (!row.r.tag.empty()) row.w += text_size(c, "[" + row.r.tag + "] ").x;
            row.w += static_cast<float>(row.r.leading.size() + row.r.trailing.size()) * (icon + gap);
            rows.push_back(std::move(row));
        }
        if (snap.legacy) legacy_line = "TS3 plugin outdated: install YapNotifier.ts3_plugin";
    }
    std::string more = overflow > 0 && cfg.show_overflow_count ? "+" + std::to_string(overflow) + " more" : "";

    float w = title_w;
    for (const auto& r : rows) w = std::max(w, r.w);
    if (!more.empty()) w = std::max(w, text_size(c, more).x);
    if (!legacy_line.empty()) w = std::max(w, text_size(c, legacy_line).x);
    float h = (title.empty() ? 0.f : title_fs + cfg.row_spacing * c.scale) + row_h * static_cast<float>(rows.size()) +
              (more.empty() ? 0.f : row_h) + (legacy_line.empty() ? 0.f : row_h);
    if (w <= 0.f || h <= 0.f) return;

    const ImVec2 origin = anchor_pos(cfg.anchor, cfg.pos_x, cfg.pos_y, w + pad * 2, h + pad * 2, c.display);
    const float align = align_of(cfg.anchor);
    if (cfg.show_panel)
        icons::panel(c.dl, origin.x, origin.y, w + pad * 2, h + pad * 2, cfg.corner_radius, alpha(cfg.panel_color, block_alpha),
                     0, 0.f);

    float y = origin.y + pad;
    const float x0 = origin.x + pad;
    if (!title.empty()) {
        float x = x0 + (w - title_w) * align;
        if (chov && chov->icon != IconShape::None) {
            icons::draw(c.dl, chov->icon, x + icon * 0.5f, y + title_fs * 0.5f, icon, alpha(chov->icon_color, block_alpha));
            x += icon + gap;
        }
        text(c, {x, y}, alpha(title_color, block_alpha), title, title_fs);
        y += title_fs + cfg.row_spacing * c.scale;
    }
    for (const auto& row : rows) {
        const float a = block_alpha * row.r.opacity;
        float x = x0 + (w - row.w) * align;
        const float cy = y + c.fs * 0.5f;
        for (const auto& ic : row.r.leading) {
            icons::draw(c.dl, ic.shape, x + icon * 0.5f, cy, icon, alpha(ic.color, a));
            x += icon + gap;
        }
        if (!row.r.tag.empty()) {
            const std::string tag = "[" + row.r.tag + "] ";
            text(c, {x, y}, alpha(row.r.tag_color, a), tag);
            x += text_size(c, tag).x;
        }
        text(c, {x, y}, alpha(row.r.name_color, a), row.name);
        x += text_size(c, row.name).x + gap;
        for (const auto& ic : row.r.trailing) {
            float ia = a;
            if (ic.shape == cfg.ind[IndSpeaking].icon && row.r.level > 0.f) {
                ia *= row.r.level;
                if (cfg.pulse) ia *= 1.f - 0.35f * (0.5f + 0.5f * std::sin(static_cast<float>(c.t) * 2.f * kPi * cfg.pulse_hz));
            }
            icons::draw(c.dl, ic.shape, x + icon * 0.5f, cy, icon, alpha(ic.color, ia));
            x += icon + gap;
        }
        y += row_h;
    }
    if (!more.empty()) {
        text(c, {x0 + (w - text_size(c, more).x) * align, y}, alpha(cfg.text_secondary, block_alpha), more);
        y += row_h;
    }
    if (!legacy_line.empty())
        text(c, {x0 + (w - text_size(c, legacy_line).x) * align, y}, alpha(rgba(255, 217, 77), block_alpha), legacy_line);
}

// --- toasts ---------------------------------------------------------------------------
void draw_toasts(const Ctx& c, State& st) {
    const Config& cfg = c.cfg;
    if (!cfg.notif_enabled || st.toasts.items().empty()) return;
    const float pad_x = 10.f * c.scale, pad_y = 6.f * c.scale, spacing = 6.f * c.scale;
    const float bar = cfg.notif_accent_bar ? 3.f * c.scale : 0.f;
    const float width = cfg.notif_width * c.scale;
    const float wrap = width - pad_x * 2 - bar;

    struct Box {
        std::string line;
        float h;
        Color col;
        float a;
    };
    std::vector<Box> boxes;
    float total = 0.f;
    for (const auto& t : st.toasts.items()) {
        Box b;
        b.line = std::string(kNotifPrefixes[t.cat]) + " " + t.text + (t.count > 1 ? " x" + std::to_string(t.count) : "");
        b.h = text_size(c, b.line, 0.f, wrap).y + pad_y * 2;
        b.col = cfg.notif[t.cat].color;
        b.a = notify::Queue::alpha(t, cfg) * c.master;
        total += b.h + spacing;
        boxes.push_back(std::move(b));
    }
    total -= spacing;
    const ImVec2 origin = anchor_pos(cfg.notif_anchor, cfg.notif_x, cfg.notif_y, width, total, c.display);
    // newest last in the vector; stack down = oldest on top.
    float y = cfg.notif_stack_up ? origin.y + total : origin.y;
    for (const auto& b : boxes) {
        if (cfg.notif_stack_up) y -= b.h;
        icons::panel(c.dl, origin.x, y, width, b.h, 2.f * c.scale, alpha(cfg.notif_background, b.a), alpha(b.col, b.a * 0.55f),
                     1.f);
        if (bar > 0.f) c.dl->AddRectFilled({origin.x, y}, {origin.x + bar, y + b.h}, alpha(b.col, b.a));
        text(c, {origin.x + bar + pad_x, y + pad_y}, alpha(cfg.text_color, b.a), b.line, 0.f, wrap);
        // prefix in the category colour on top of the body text
        const size_t sp = b.line.find(' ');
        text(c, {origin.x + bar + pad_x, y + pad_y}, alpha(b.col, b.a), b.line.substr(0, sp));
        y += cfg.notif_stack_up ? -spacing : b.h + spacing;
    }
}

// --- chat feed -------------------------------------------------------------------
void draw_chat(const Ctx& c, State& st, uint64_t now_ms) {
    const Config& cfg = c.cfg;
    if (!cfg.chat_enabled) return;
    std::vector<const notify::ChatLine*> lines;
    for (auto it = st.chat.rbegin(); it != st.chat.rend() && lines.size() < static_cast<size_t>(cfg.chat_max_visible); ++it)
        if (notify::chat_visible(*it, cfg, now_ms)) lines.push_back(&*it);
    if (lines.empty()) return;
    if (!cfg.chat_newest_top) std::reverse(lines.begin(), lines.end());

    const float pad = 6.f * c.scale, width = cfg.chat_width * c.scale, wrap = width - pad * 2;
    const float small = c.fs * 0.95f;
    struct L {
        std::string prefix, body;
        float h;
    };
    std::vector<L> laid;
    float total = pad * 2;
    for (const auto* l : lines) {
        L x;
        char ts[8] = {};
        if (cfg.chat_timestamp) std::snprintf(ts, sizeof ts, "%02d:%02d ", l->hour, l->minute);
        x.prefix = ts;
        x.prefix += l->cat == notify::ChatCat::Channel ? "# " : l->cat == notify::ChatCat::Private ? "@ " : "! ";
        if (cfg.chat_channel_name && !l->channel.empty()) x.prefix += "[" + l->channel + "] ";
        if (cfg.chat_sender) x.prefix += l->sender + ": ";
        x.body = l->text;
        const float pw = text_size(c, x.prefix, small).x;
        x.h = pw + text_size(c, x.body, small).x <= wrap
                  ? small
                  : text_size(c, x.prefix + x.body, small, wrap).y;
        total += x.h + 2.f * c.scale;
        laid.push_back(std::move(x));
    }
    const ImVec2 origin = anchor_pos(cfg.chat_anchor, cfg.chat_x, cfg.chat_y, width, total, c.display);
    icons::panel(c.dl, origin.x, origin.y, width, total, 4.f * c.scale, alpha(cfg.chat_background, c.master), 0, 0.f);
    float y = origin.y + pad;
    for (const auto& l : laid) {
        const float pw = text_size(c, l.prefix, small).x;
        text(c, {origin.x + pad, y}, alpha(cfg.chat_sender_color, c.master), l.prefix, small);
        if (pw + text_size(c, l.body, small).x <= wrap) {
            text(c, {origin.x + pad + pw, y}, alpha(cfg.text_color, c.master), l.body, small);
        } else {
            // Wrapped: draw prefix+body together so the wrap accounts for the prefix, body colour wins.
            text(c, {origin.x + pad, y}, alpha(cfg.text_color, c.master), l.prefix + l.body, small, wrap);
            text(c, {origin.x + pad, y}, alpha(cfg.chat_sender_color, c.master), l.prefix, small);
        }
        y += l.h + 2.f * c.scale;
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

void draw(State& st, const Config& cfg, const ts::Snapshot& snap, float dt_ms, uint64_t now_ms) {
    st.env.tick(snap.users, dt_ms, cfg);
    st.toasts.tick(dt_ms, cfg);

    bool busy = st.env.any_active() || !st.toasts.items().empty();
    st.idle_ms = busy ? 0.f : st.idle_ms + dt_ms;
    const float target = cfg.fade_when_idle && st.idle_ms > static_cast<float>(cfg.idle_after_ms) ? 1.f : 0.f;
    st.idle_fade += (target - st.idle_fade) * std::min(1.f, dt_ms / 400.f);

    ImFont* font = ImGui::GetFont();
    Ctx c{ImGui::GetBackgroundDrawList(),
          font,
          font->FontSize * cfg.scale,
          cfg.scale,
          cfg.master_opacity * (1.f - st.idle_fade * (1.f - cfg.idle_opacity)),
          ImGui::GetIO().DisplaySize,
          cfg,
          static_cast<double>(now_ms) / 1000.0};
    draw_roster(c, st, snap);
    draw_toasts(c, st);
    draw_chat(c, st, now_ms);
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
