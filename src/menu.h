#pragma once
// The INSERT menu: menu.rml (assets/ui) bound to the live Config through the "cfg"
// data model; edits apply immediately, Save writes the INI.
#include <string>

#include "config.h"
#include "hud.h"

namespace Rml {
class Context;
}

namespace yap::menu {

struct Host {
    Config& cfg;
    const std::wstring& ini;
    bool open = false;  // cleared by the window's [x]
    hud::State& hud;
};

// Registers the data model and loads menu.rml (hidden). False = logged, overlay dormant.
bool init(Host& h, Rml::Context& ctx);
void show(bool open);
// Refreshes the live facts (connection, roster, profiles) while the menu is open.
void sync(Host& h);

}  // namespace yap::menu
