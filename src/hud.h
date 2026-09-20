#pragma once
// Everything drawn while the menu is closed: channel title, roster, toasts and
// the chat feed. Draws straight into ImGui's background draw list, so nothing
// here is ever hit-testable.
#include <cstdint>
#include <vector>

#include "config.h"
#include "notify.h"
#include "roster.h"
#include "teamspeak.h"

namespace yap::hud {

struct State {
    roster::Envelope env;
    notify::Queue toasts;
    std::vector<notify::ChatLine> chat;  // oldest first, capped at chat_history
    float idle_ms = 0.f;
    float idle_fade = 0.f;  // 0 = fully visible, 1 = at idle_opacity
    int prev_conn = -1;
    uint64_t demo_next_ms = 0;
    int demo_step = 0;
};

// Routes plugin events into toasts and the chat log.
void feed(State& st, const Config& cfg, const std::vector<ts::Event>& events, uint64_t now_ms);

void draw(State& st, const Config& cfg, const ts::Snapshot& snap, float dt_ms, uint64_t now_ms);

// Demo mode: a fixed roster showing every state plus a scripted event stream.
const ts::Snapshot& demo_snapshot();
void demo_tick(State& st, const Config& cfg, uint64_t now_ms);

}  // namespace yap::hud
