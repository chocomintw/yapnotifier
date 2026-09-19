#pragma once
#include <Windows.h>

#include <string>

#include "config.h"

struct IDXGISwapChain;

// ImGui overlay: owns the ImGui context, the backbuffer RTV, and the game
// window's WndProc. Everything here except shutdown() runs on the render
// thread inside the Present/ResizeBuffers hooks.
namespace yap::overlay {

// Call once before hooks are enabled.
void set_config(const Config& cfg, std::wstring ini_path);

// From the Present hook, before the original Present.
void render(IDXGISwapChain* swapchain);

// From the ResizeBuffers hook, before the original ResizeBuffers.
void on_resize();

// From the eject thread, after hooks are disabled and drained.
void shutdown();

}  // namespace yap::overlay
