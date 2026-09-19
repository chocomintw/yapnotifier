#pragma once

// MinHook-based detours on IDXGISwapChain::Present / ::ResizeBuffers.
namespace yap::hooks {

// Resolves the swapchain vtable via a throwaway D3D11 device, then creates and
// enables both hooks. Returns false (and leaves nothing installed) on failure.
bool install();

// Disables the hooks, waits for in-flight calls to leave our detours, and
// tears MinHook down. Safe to call even if install() failed.
void remove();

}  // namespace yap::hooks
