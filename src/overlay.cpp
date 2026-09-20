#include "overlay.h"

#include "log.h"
#include "teamspeak.h"
#include "update.h"
#include "version.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using namespace yap;

// Pure-black is the transparency key: anything we clear/leave black shows the game
// through it (LWA_COLORKEY also makes those pixels click-through for free). ImGui's
// default window background is ~RGB(15,15,15), not pure black, so panels stay visible.
// ponytail: color-key = binary transparency (no true per-pixel alpha over the game);
//           upgrade to a DirectComposition flip-model swapchain if soft edges matter.
constexpr COLORREF kColorKey = RGB(0, 0, 0);
constexpr UINT WM_YAP_TOGGLE = WM_APP + 1;
constexpr wchar_t kGameWindowClass[] = L"grcWindow";

Config g_cfg;
std::wstring g_ini;

HWND g_hwnd = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_width = 0, g_height = 0;

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_menu_open{false};

// --- camera lock (borrowed from vlights) -------------------------------------
// The game reads the mouse via process-wide raw input (RIDEV_INPUTSINK), so it
// keeps turning the camera while we drag a slider. While the menu is open we
// unregister the game's raw mouse and restore its exact registration on close.
std::vector<RAWINPUTDEVICE> g_saved_mouse;
bool g_mouse_suspended = false;

void suspend_game_mouse() {
    if (g_mouse_suspended) return;
    UINT n = 0;
    if (GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE)) != 0 || n == 0) return;
    std::vector<RAWINPUTDEVICE> all(n);
    UINT got = GetRegisteredRawInputDevices(all.data(), &n, sizeof(RAWINPUTDEVICE));
    if (got == static_cast<UINT>(-1)) return;
    g_saved_mouse.clear();
    for (UINT i = 0; i < got; ++i)
        if (all[i].usUsagePage == 0x01 && all[i].usUsage == 0x02) g_saved_mouse.push_back(all[i]);
    if (g_saved_mouse.empty()) return;
    RAWINPUTDEVICE remove{0x01, 0x02, RIDEV_REMOVE, nullptr};
    if (RegisterRawInputDevices(&remove, 1, sizeof(remove))) {
        g_mouse_suspended = true;
        log::info("overlay: camera locked (raw mouse suspended)");
    }
}

void restore_game_mouse() {
    if (!g_mouse_suspended) return;
    RegisterRawInputDevices(g_saved_mouse.data(), static_cast<UINT>(g_saved_mouse.size()), sizeof(RAWINPUTDEVICE));
    g_mouse_suspended = false;
    log::info("overlay: camera unlocked");
}

// --- D3D --------------------------------------------------------------------
template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool create_rtv() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return SUCCEEDED(hr) && g_rtv;
}

bool create_device(HWND hwnd, UINT w, UINT h) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, nullptr, &g_ctx);
    if (FAILED(hr)) {
        log::error("overlay: D3D11CreateDeviceAndSwapChain failed: {:#x}", static_cast<unsigned>(hr));
        return false;
    }
    g_width = w;
    g_height = h;
    return create_rtv();
}

void destroy_device() {
    release(g_rtv);
    release(g_swapchain);
    release(g_ctx);
    release(g_device);
}

// --- window ------------------------------------------------------------------
// Screen rect of the game's client area, so our window covers exactly it.
bool game_client_rect(RECT& out) {
    HWND game = FindWindowW(kGameWindowClass, nullptr);
    if (!game) return false;
    RECT c{};
    if (!GetClientRect(game, &c) || c.right <= c.left || c.bottom <= c.top) return false;
    POINT tl{c.left, c.top};
    ClientToScreen(game, &tl);
    out = {tl.x, tl.y, tl.x + (c.right - c.left), tl.y + (c.bottom - c.top)};
    return true;
}

void set_click_through(bool on) {
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
        case WM_YAP_TOGGLE: {
            bool open = !g_menu_open.load();
            g_menu_open = open;
            set_click_through(!open);
            if (open) {
                SetForegroundWindow(h);
                SetFocus(h);
                suspend_game_mouse();
            } else {
                restore_game_mouse();
                if (HWND game = FindWindowW(kGameWindowClass, nullptr)) SetForegroundWindow(game);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(nullptr);  // ImGui draws its own cursor
                return TRUE;
            }
            break;
        case WM_DESTROY:
            restore_game_mouse();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// --- ImGui content -----------------------------------------------------------
void draw_talkers() {
    auto snap = ts::snapshot();
    if (snap->talking.empty()) return;
    ImGui::SetNextWindowPos({g_cfg.pos_x, g_cfg.pos_y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(g_cfg.opacity);
    ImGui::Begin("##yap_talkers", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
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

void draw_notice() {
    auto notice = update::notice();
    if (notice->empty()) return;
    static ULONGLONG first_seen = 0;
    if (!first_seen) first_seen = GetTickCount64();
    if (GetTickCount64() - first_seen > 20000) return;
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x * 0.5f, 24.f}, ImGuiCond_Always, {0.5f, 0.f});
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##yap_notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextColored({1.f, 0.85f, 0.3f, 1.f}, "%s", notice->c_str());
    ImGui::End();
}

void draw_menu() {
    ImGui::SetNextWindowSize({420, 0}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({g_cfg.pos_x, g_cfg.pos_y + 80}, ImGuiCond_FirstUseEver);
    bool open = g_menu_open.load();
    ImGui::Begin("YapNotifier v" YAP_VERSION, &open, ImGuiWindowFlags_AlwaysAutoResize);
    auto snap = ts::snapshot();
    ImGui::TextDisabled(snap->connected ? "TS3 plugin: connected"
                                        : "TS3 plugin: not connected (enable the YapNotifier plugin in TeamSpeak)");
    if (auto n = update::notice(); !n->empty()) ImGui::TextColored({1.f, 0.85f, 0.3f, 1.f}, "%s", n->c_str());
    ImGui::Separator();
    ImGui::InputInt("UDP port", &g_cfg.port);
    ImGui::Checkbox("Auto-update on launch", &g_cfg.auto_update);
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
    ImGui::TextDisabled("INSERT toggles this menu, END ejects the plugin");
    ImGui::End();
    if (!open && g_menu_open.load()) PostMessageW(g_hwnd, WM_YAP_TOGGLE, 0, 0);  // window's [x]
}

// Keep our window aligned with the game and sized to its client area.
void track_game_window() {
    RECT r{};
    if (!game_client_rect(r)) return;
    UINT w = static_cast<UINT>(r.right - r.left), h = static_cast<UINT>(r.bottom - r.top);
    SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE);
    if ((w != g_width || h != g_height) && w && h) {
        release(g_rtv);
        if (SUCCEEDED(g_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
            g_width = w;
            g_height = h;
            create_rtv();
        }
    }
}

void render_frame() {
    if (!g_rtv) return;
    if (g_menu_open.load()) {
        ClipCursor(nullptr);
        ImGui::GetIO().MouseDrawCursor = true;
    } else {
        ImGui::GetIO().MouseDrawCursor = false;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_talkers();
    draw_notice();
    if (g_menu_open.load()) draw_menu();
    ImGui::Render();
    // Black = transparent via color key, and keyed pixels are also *not hit-testable*: a
    // drag whose cursor outruns the menu by a frame lands on the game and ImGui loses the
    // move. While the menu is open clear to RGB(1,1,1) instead: invisibly dim, but every
    // pixel is ours, so all mouse input (and SetCapture) reaches ImGui.
    const float k = g_menu_open.load() ? 1.f / 255.f : 0.f;
    const float clear[4] = {k, k, k, 1.f};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapchain->Present(0, 0);  // no vsync: never contend with the game's swapchain
}

DWORD WINAPI ui_thread(LPVOID) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"YapNotifierOverlay";
    if (!RegisterClassExW(&wc)) {
        log::error("overlay: RegisterClassEx failed ({})", GetLastError());
        return 0;
    }

    RECT r{};
    if (!game_client_rect(r)) {
        r = {0, 0, static_cast<LONG>(GetSystemMetrics(SM_CXSCREEN)),
             static_cast<LONG>(GetSystemMetrics(SM_CYSCREEN))};
    }
    UINT w = static_cast<UINT>(r.right - r.left), h = static_cast<UINT>(r.bottom - r.top);

    // Layered + transparent + topmost + no-activate tool window: draws over the game,
    // starts click-through, never steals focus until the menu opens.
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"YapNotifier", WS_POPUP, r.left, r.top, static_cast<int>(w), static_cast<int>(h),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        log::error("overlay: CreateWindowEx failed ({})", GetLastError());
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    SetLayeredWindowAttributes(g_hwnd, kColorKey, 0, LWA_COLORKEY);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    if (!create_device(g_hwnd, w, h)) {
        destroy_device();
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_ctx)) {
        log::error("overlay: ImGui backend init failed");
        ImGui::DestroyContext();
        destroy_device();
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    g_ready = true;
    log::info("overlay: ready (own {}x{} window, no game hooks)", w, h);

    MSG msg{};
    while (!g_stop.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        track_game_window();
        render_frame();
        Sleep(16);  // ~60fps; ponytail: could idle when nothing is talking + menu closed
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    log::info("overlay: shut down");
    return 0;
}
}  // namespace

namespace yap::overlay {

void set_config(const Config& cfg, std::wstring ini_path) {
    g_cfg = cfg;
    g_ini = std::move(ini_path);
}

void start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread([] { ui_thread(nullptr); });
}

void stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

void toggle_menu() {
    if (g_ready.load() && g_hwnd) PostMessageW(g_hwnd, WM_YAP_TOGGLE, 0, 0);
}

}  // namespace yap::overlay
