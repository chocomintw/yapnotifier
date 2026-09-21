#pragma once
#include <string>

#include "config.h"

// The overlay is a separate, transparent, top-most, click-through window with its
// OWN D3D11 device — it does NOT hook the game's renderer or subclass the game
// window. FiveM's anti-cheat (adhesive) terminates the process a minute or so
// after any IDXGISwapChain::Present hook or game-window subclass, so we own our
// window and touch nothing of the game's. Approach borrowed from tcpstorm/vlights.
namespace yap::overlay {

// Call before start().
void set_config(const Config& cfg, std::wstring ini_path);

// Spawns the UI thread (window + device + RmlUi + message loop). Never throws;
// on failure it logs and the overlay stays dormant.
void start();

// Stops the UI thread and tears everything down. Safe if start() failed.
void stop();

// Toggle the interactive config menu (from the hotkey poll on another thread).
void toggle_menu();

}  // namespace yap::overlay
