#include "notify.h"

#include <algorithm>

namespace yap::notify {

std::string format_template(std::string_view fmt, Vars vars) {
    std::string out;
    out.reserve(fmt.size() + 32);
    while (!fmt.empty()) {
        size_t open = fmt.find('{');
        if (open == std::string_view::npos) {
            out += fmt;
            break;
        }
        out += fmt.substr(0, open);
        size_t close = fmt.find('}', open);
        if (close == std::string_view::npos) {
            out += fmt.substr(open);
            break;
        }
        std::string_view key = fmt.substr(open + 1, close - open - 1);
        bool hit = false;
        for (const auto& [k, v] : vars) {
            if (k != key) continue;
            out += v;
            hit = true;
            break;
        }
        if (!hit) out += fmt.substr(open, close - open + 1);
        fmt.remove_prefix(close + 1);
    }
    return out;
}

namespace {
std::string_view status_word(std::string_view s) {
    if (s == "connection_lost") return "connection lost";
    if (s == "server_shutdown") return "server shut down";
    return s;
}
}  // namespace

bool toast_for(const ts::Event& e, const Config& cfg, Notif& cat, std::string& text) {
    using K = ts::Event;
    std::string_view name, channel, from, to, previous, count, status, server, message;
    switch (e.kind) {
        case K::Join:
            cat = NotifJoin;
            name = e.f[1];
            from = previous = e.f[0].empty() ? "the server" : std::string_view(e.f[0]);
            break;
        case K::Leave:
            cat = NotifLeave;
            name = e.f[1];
            to = e.f[0].empty() ? "the server" : std::string_view(e.f[0]);
            break;
        case K::Switch:
            cat = NotifSwitch;
            count = e.f[0];
            previous = e.f[1];
            channel = e.f[2];
            break;
        case K::Conn:
            cat = NotifConnection;
            server = e.f[0];
            status = status_word(e.f[1]);
            break;
        case K::Whisper:
            cat = NotifWhisper;
            channel = e.f[0].empty() ? "your channel" : std::string_view(e.f[0]);
            name = e.f[1];
            break;
        case K::Chat:
            cat = e.f[0] == "private" ? NotifPrivateChat : e.f[0] == "poke" ? NotifPoke : NotifChat;
            name = e.f[1];
            channel = e.f[2];
            message = e.f[3];
            break;
    }
    if (!cfg.notif[cat].enabled) return false;
    text = format_template(cfg.notif[cat].format, {{"name", name},
                                                   {"channel", channel},
                                                   {"from", from},
                                                   {"to", to},
                                                   {"previous", previous},
                                                   {"count", count},
                                                   {"status", status},
                                                   {"server", server},
                                                   {"message", message}});
    return !text.empty();
}

void Queue::push(Notif cat, std::string text, const Config& cfg) {
    if (since_connect_ms_ >= 0.f && since_connect_ms_ < static_cast<float>(cfg.notif_suppress_after_connect_ms) &&
        (cat == NotifJoin || cat == NotifLeave))
        return;  // the roster flood right after connecting is not news
    if (cfg.notif_merge_duplicates && !items_.empty()) {
        Toast& last = items_.back();
        const float in = static_cast<float>(cfg.notif_in_ms);
        const float end_hold = in + static_cast<float>(cfg.notif[last.cat].hold_ms);
        if (last.cat == cat && last.text == text && last.age_ms < end_hold) {
            ++last.count;
            last.age_ms = std::min(last.age_ms, in);  // restart the hold
            return;
        }
    }
    items_.push_back({cat, std::move(text), 1, 0.f});
    while (items_.size() > static_cast<size_t>(cfg.notif_max_visible)) items_.erase(items_.begin());
}

void Queue::tick(float dt_ms, const Config& cfg) {
    if (since_connect_ms_ >= 0.f) since_connect_ms_ += dt_ms;
    for (auto& t : items_) t.age_ms += dt_ms;
    std::erase_if(items_, [&](const Toast& t) {
        return t.age_ms >= static_cast<float>(cfg.notif_in_ms + cfg.notif[t.cat].hold_ms + cfg.notif_out_ms);
    });
}

void Queue::mark_connected() { since_connect_ms_ = 0.f; }

float Queue::alpha(const Toast& t, const Config& cfg) {
    const float in = static_cast<float>(cfg.notif_in_ms);
    const float hold = static_cast<float>(cfg.notif[t.cat].hold_ms);
    const float out = static_cast<float>(cfg.notif_out_ms);
    if (t.age_ms < in) return in > 0.f ? t.age_ms / in : 1.f;
    if (t.age_ms < in + hold) return 1.f;
    if (out <= 0.f) return 0.f;
    return std::max(0.f, 1.f - (t.age_ms - in - hold) / out);
}

bool chat_line_for(const ts::Event& e, ChatLine& out) {
    if (e.kind != ts::Event::Chat) return false;
    out.cat = e.f[0] == "server"    ? ChatCat::Server
              : e.f[0] == "private" ? ChatCat::Private
              : e.f[0] == "poke"    ? ChatCat::Poke
                                    : ChatCat::Channel;
    out.sender = e.f[1];
    out.channel = e.f[2];
    out.text = e.f[3];
    return true;
}

bool chat_visible(const ChatLine& l, const Config& cfg, uint64_t now_ms) {
    const bool cat_on = l.cat == ChatCat::Channel   ? cfg.chat_channel
                        : l.cat == ChatCat::Server  ? cfg.chat_server
                        : l.cat == ChatCat::Private ? cfg.chat_private
                                                    : cfg.chat_poke;
    if (!cat_on) return false;
    if (cfg.chat_retention_s > 0 && now_ms - l.at_ms > static_cast<uint64_t>(cfg.chat_retention_s) * 1000) return false;
    return true;
}

}  // namespace yap::notify
