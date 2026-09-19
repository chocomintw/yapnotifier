#pragma once

// MinHook-based detours on IDXGISwapChain::Present / ::ResizeBuffers.
namespace yap::hooks {

// Resolves the swapchain vtable via a throwaway D3D11 device, then creates and
// enables both hooks. Returns false (and leaves nothing installed) on failure.
// safe_mode (INI diagnostic): hook Present only and pass every call straight
// through — no overlay, no ResizeBuffers hook. Used to tell an anti-cheat /
// inline-hook crash apart from a bug in our own render path.
bool install(bool safe_mode, bool hook_resize);

// Disables the hooks, waits for in-flight calls to leave our detours, and
// tears MinHook down. Safe to call even if install() failed.
void remove();

}  // namespace yap::hooks
