#pragma once
// Everything shown while the menu is closed: channel title, roster, toasts, the
// chat feed and the updater banner. hud.rml (assets/ui) renders a data model
// that sync() refills every frame. Nothing here is hit-testable except in edit
// mode (menu open), where the blocks can be dragged to set their offsets.
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "notify.h"
#include "roster.h"
#include "teamspeak.h"

namespace Rml {
class Context;
}

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

// Registers the "hud" data model and loads hud.rml. False = logged, overlay dormant.
bool init(Rml::Context& ctx);

// Edit mode: while `cfg` is set every block is shown and draggable, and a drag writes the
// new offset straight into it (the menu's sliders follow). nullptr ends it.
void edit(Config* cfg);

// Ticks the animations and republishes the view; `notice` is the updater banner or "".
void sync(State& st, const Config& cfg, const ts::Snapshot& snap, float dt_ms, uint64_t now_ms, const std::string& notice);

// Demo mode: a fixed roster showing every state plus a scripted event stream.
const ts::Snapshot& demo_snapshot();
void demo_tick(State& st, const Config& cfg, uint64_t now_ms);

}  // namespace yap::hud
