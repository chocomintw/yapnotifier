#pragma once
// Toast queue, chat log and template formatting. Pure logic (no UI library) so the
// host-side test can exercise the lifecycle.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h"
#include "teamspeak.h"

namespace yap::notify {

using Vars = std::initializer_list<std::pair<std::string_view, std::string_view>>;
// "{name} joined {channel}" -> substituted; unknown placeholders stay literal.
std::string format_template(std::string_view fmt, Vars vars);

// Turns a plugin event into (category, text). Returns false when the category is
// disabled or the event carries nothing to show.
bool toast_for(const ts::Event& e, const Config& cfg, Notif& cat, std::string& text);

struct Toast {
    Notif cat = NotifJoin;
    std::string text;
    int count = 1;
    float age_ms = 0.f;
};

class Queue {
public:
    void push(Notif cat, std::string text, const Config& cfg);
    void tick(float dt_ms, const Config& cfg);
    void mark_connected();  // starts the post-connect suppression window
    void clear() { items_.clear(); }

    // 0..1 alpha for the fade in / hold / fade out envelope.
    static float alpha(const Toast& t, const Config& cfg);
    const std::vector<Toast>& items() const { return items_; }

private:
    std::vector<Toast> items_;
    float since_connect_ms_ = -1.f;
};

enum class ChatCat { Channel, Server, Private, Poke };
struct ChatLine {
    ChatCat cat = ChatCat::Channel;
    std::string sender, channel, text;
    uint64_t at_ms = 0;  // GetTickCount64 at receipt
    int hour = 0, minute = 0;
};
bool chat_line_for(const ts::Event& e, ChatLine& out);  // false unless e is a chat event
bool chat_visible(const ChatLine& l, const Config& cfg, uint64_t now_ms);

}  // namespace yap::notify
