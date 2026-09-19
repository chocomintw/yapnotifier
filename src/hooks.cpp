#include "hooks.h"

#include "log.h"
#include "overlay.h"

#include <MinHook.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>

namespace {
using Microsoft::WRL::ComPtr;
using namespace yap;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// IDXGISwapChain vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7).
constexpr size_t kPresentSlot = 8;
constexpr size_t kResizeBuffersSlot = 13;

PresentFn g_present_orig = nullptr;
ResizeBuffersFn g_resize_orig = nullptr;
bool g_installed = false;

// Counts threads currently inside a detour so remove() can drain them.
std::atomic<int> g_in_flight{0};

struct InFlight {
    InFlight() { ++g_in_flight; }
    ~InFlight() { --g_in_flight; }
};

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    InFlight guard;
    if (!(flags & DXGI_PRESENT_TEST)) overlay::render(sc);
    return g_present_orig(sc, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT fmt, UINT flags) {
    InFlight guard;
    overlay::on_resize();
    return g_resize_orig(sc, count, w, h, fmt, flags);
}

// Every IDXGISwapChain in the process shares dxgi.dll's vtable, so the
// function pointers off a throwaway swapchain are the game's too. We use our
// own hidden window so this works before the game window exists.
bool resolve_vtable(void*& present, void*& resize_buffers) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"YapNotifierDummy";
    if (!RegisterClassExW(&wc)) return false;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 2, 2,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (SUCCEEDED(hr)) {
        void** vtable = *reinterpret_cast<void***>(sc.Get());
        present = vtable[kPresentSlot];
        resize_buffers = vtable[kResizeBuffersSlot];
    } else {
        log::error("hooks: dummy D3D11CreateDeviceAndSwapChain failed: {:#x}", static_cast<unsigned>(hr));
    }
    sc.Reset();
    ctx.Reset();
    dev.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return SUCCEEDED(hr);
}
}  // namespace

namespace yap::hooks {

bool install() {
    if (MH_Initialize() != MH_OK) {
        log::error("hooks: MH_Initialize failed");
        return false;
    }
    void* present = nullptr;
    void* resize = nullptr;
    if (!resolve_vtable(present, resize)) {
        MH_Uninitialize();
        return false;
    }
    if (MH_CreateHook(present, &hk_present, reinterpret_cast<void**>(&g_present_orig)) != MH_OK ||
        MH_CreateHook(resize, &hk_resize_buffers, reinterpret_cast<void**>(&g_resize_orig)) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log::error("hooks: creating/enabling hooks failed");
        MH_Uninitialize();
        return false;
    }
    g_installed = true;
    log::info("hooks: Present={:#x} ResizeBuffers={:#x}",
              reinterpret_cast<uintptr_t>(present), reinterpret_cast<uintptr_t>(resize));
    return true;
}

void remove() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    // ponytail: bounded spin; a detour stuck >1s means something worse than a leak.
    for (int i = 0; i < 100 && g_in_flight > 0; ++i) Sleep(10);
    MH_Uninitialize();
    g_installed = false;
    log::info("hooks: removed");
}

}  // namespace yap::hooks
