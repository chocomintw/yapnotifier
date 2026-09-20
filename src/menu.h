#pragma once
// The INSERT menu: tabbed ImGui window editing the live Config.
#include <string>

#include "config.h"
#include "hud.h"

namespace yap::menu {

struct Host {
    Config& cfg;
    const std::wstring& ini;
    bool open = true;  // set false by the window's [x]
    hud::State& hud;
};

void draw(Host& h);

}  // namespace yap::menu
