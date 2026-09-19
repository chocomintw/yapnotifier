#include "overlay.h"

#include "log.h"
#include "teamspeak.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using Microsoft::WRL::ComPtr;
using namespace yap;

Config g_cfg;
std::wstring g_ini;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<ID3D11RenderTargetView> g_rtv;
HWND g_hwnd = nullptr;
WNDPROC g_wndproc_orig = nullptr;

bool g_inited = false;
bool g_failed = false;  // sticky: after an init failure, Present is pass-through forever
bool g_menu_open = false;

bool is_input_msg(UINT m) {
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) ||
           (m >= WM_KEYFIRST && m <= WM_KEYLAST) ||
           m == WM_INPUT;  // GTA reads mouse-look via raw input
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    if (g_menu_open && is_input_msg(m)) return 0;  // menu owns input; game sees nothing
    return CallWindowProcW(g_wndproc_orig, h, m, w, l);
}

bool init(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
    g_device->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;
    g_hwnd = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our own INI handles persistence
    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_device.Get(), g_ctx.Get())) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    g_wndproc_orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    log::info("overlay: initialised (hwnd={:#x}, {}x{})",
              reinterpret_cast<uintptr_t>(g_hwnd), desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

bool create_rtv(IDXGISwapChain* sc) {
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;
    return SUCCEEDED(g_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &g_rtv));
}

void poll_menu_key() {
    static bool was_down = false;
    bool down = (GetAsyncKeyState(g_cfg.menu_key) & 0x8000) != 0;
    if (down && !was_down) g_menu_open = !g_menu_open;
    was_down = down;
    ImGui::GetIO().MouseDrawCursor = g_menu_open;  // game hides the cursor; draw our own
}

void draw_talkers() {
    auto snap = ts::snapshot();
    if (snap->talking.empty()) return;

    ImGui::SetNextWindowPos({g_cfg.pos_x, g_cfg.pos_y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(g_cfg.opacity);
    ImGui::Begin("##yap_talkers", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(g_cfg.scale);
    const float r = ImGui::GetTextLineHeight() * 0.3f;
    for (const auto& c : snap->talking) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            {p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f}, r, IM_COL32(80, 220, 80, 255));
        ImGui::Dummy({r * 2 + 6.f, 0.f});
        ImGui::SameLine();
        ImGui::TextUnformatted(c.nickname.c_str());
    }
    ImGui::End();
}

void draw_menu() {
    ImGui::Begin("YapNotifier", &g_menu_open, ImGuiWindowFlags_AlwaysAutoResize);
    auto snap = ts::snapshot();
    ImGui::TextDisabled(snap->connected ? "TS3 plugin: connected" : "TS3 plugin: not connected (is the YapNotifier plugin enabled in TeamSpeak?)");
    ImGui::Separator();
    ImGui::InputInt("UDP port", &g_cfg.port);
    ImGui::Separator();
    ImGui::SliderFloat("X", &g_cfg.pos_x, 0.f, ImGui::GetIO().DisplaySize.x);
    ImGui::SliderFloat("Y", &g_cfg.pos_y, 0.f, ImGui::GetIO().DisplaySize.y);
    ImGui::SliderFloat("Opacity", &g_cfg.opacity, 0.f, 1.f);
    ImGui::SliderFloat("Scale", &g_cfg.scale, 0.5f, 3.f);
    ImGui::Separator();
    if (ImGui::Button("Save")) {
        config::save(g_cfg, g_ini);
        ts::configure(g_cfg.port);
        log::info("overlay: config saved");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(INSERT toggles this menu, END ejects the plugin)");
    ImGui::End();
}
}  // namespace

namespace yap::overlay {

void set_config(const Config& cfg, std::wstring ini_path) {
    g_cfg = cfg;
    g_ini = std::move(ini_path);
}

void render(IDXGISwapChain* sc) {
    if (g_failed) return;
    if (!g_inited) {
        if (!init(sc)) {
            g_failed = true;
            log::error("overlay: init failed; overlay disabled");
            return;
        }
        g_inited = true;
    }
    if (!g_rtv && !create_rtv(sc)) {
        g_failed = true;
        log::error("overlay: could not create backbuffer RTV; overlay disabled");
        return;
    }

    poll_menu_key();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_talkers();
    if (g_menu_open) draw_menu();
    ImGui::Render();
    g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void on_resize() {
    // Must drop every backbuffer reference before the original ResizeBuffers
    // runs or it fails with DXGI_ERROR_INVALID_CALL. Recreated lazily in render().
    g_rtv.Reset();
    if (g_inited) ImGui_ImplDX11_InvalidateDeviceObjects();
}

void shutdown() {
    if (!g_inited) return;
    if (g_wndproc_orig) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_wndproc_orig));
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_rtv.Reset();
    g_ctx.Reset();
    g_device.Reset();
    g_inited = false;
    log::info("overlay: shut down");
}

}  // namespace yap::overlay
