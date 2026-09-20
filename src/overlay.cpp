#include "overlay.h"

#include "hud.h"
#include "log.h"
#include "menu.h"
#include "teamspeak.h"
#include "theme.h"
#include "update.h"

#include <Windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using namespace yap;

// Per-pixel alpha over the game via a DirectComposition swapchain on a layered window
// whose own (never painted) surface is made transparent by extending the DWM frame.
// LWA_ALPHA 255 means hit-testing is whole-window, so click-through is purely
// WS_EX_TRANSPARENT, toggled with the menu. (LWA_COLORKEY was visually right but user32
// hit-tested the never-painted GDI bitmap, so every click fell through; and
// WS_EX_TRANSPARENT only passes input through when the window is also WS_EX_LAYERED.)
constexpr UINT WM_YAP_TOGGLE = WM_APP + 1;
constexpr wchar_t kGameWindowClass[] = L"grcWindow";

Config g_cfg;
std::wstring g_ini;
hud::State g_hud;
ULONGLONG g_last_frame = 0;

HWND g_hwnd = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain1* g_swapchain = nullptr;
IDCompositionDevice* g_dcomp = nullptr;
IDCompositionTarget* g_dcomp_target = nullptr;
IDCompositionVisual* g_dcomp_visual = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_width = 0, g_height = 0;

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_visible{true};  // game in foreground: window shown + rendered

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
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx);
    IDXGIDevice* dxgi = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(hr)) hr = g_device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = factory->CreateSwapChainForComposition(dxgi, &sd, nullptr, &g_swapchain);
    }
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi, IID_PPV_ARGS(&g_dcomp));
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_dcomp_target);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateVisual(&g_dcomp_visual);
    if (SUCCEEDED(hr)) hr = g_dcomp_visual->SetContent(g_swapchain);
    if (SUCCEEDED(hr)) hr = g_dcomp_target->SetRoot(g_dcomp_visual);
    if (SUCCEEDED(hr)) hr = g_dcomp->Commit();
    release(factory);
    release(dxgi);
    if (FAILED(hr)) {
        log::error("overlay: D3D11/DirectComposition setup failed: {:#x}", static_cast<unsigned>(hr));
        return false;
    }
    g_width = w;
    g_height = h;
    return create_rtv();
}

void destroy_device() {
    release(g_rtv);
    release(g_dcomp_visual);
    release(g_dcomp_target);
    release(g_dcomp);
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

// `font_file` (relative to the plugins dir) if set, else Quicksand from the RCDATA
// resource in YapNotifier.rc. Falls back to ImGui's built-in ProggyClean if anything
// is off, so a bad font never blanks the overlay.
void load_font() {
    if (!g_cfg.font_file.empty()) {
        std::filesystem::path p = g_cfg.font_file;
        if (p.is_relative()) p = std::filesystem::path(g_ini).parent_path() / p;
        if (ImGui::GetIO().Fonts->AddFontFromFileTTF(p.string().c_str(), g_cfg.font_size)) return;
        log::error("overlay: could not load font {}, using Quicksand", p.string());
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&load_font), &self);
    HRSRC res = self ? FindResourceW(self, L"YAP_FONT", RT_RCDATA) : nullptr;
    HGLOBAL blob = res ? LoadResource(self, res) : nullptr;
    void* data = blob ? LockResource(blob) : nullptr;
    DWORD size = res ? SizeofResource(self, res) : 0;
    if (!data || !size) {
        log::error("overlay: font resource missing, using ImGui default");
        return;
    }
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;  // resource memory belongs to the module, never freed
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(size), g_cfg.font_size, &fc);
}

// --- ImGui content -----------------------------------------------------------
void draw_hud(float dt_ms, ULONGLONG now) {
    static std::vector<ts::Event> events;
    events.clear();
    if (g_cfg.demo) {
        hud::demo_tick(g_hud, g_cfg, now);
        std::vector<ts::Event> dropped;
        ts::drain_events(dropped);  // keep the real queue from piling up meanwhile
        hud::draw(g_hud, g_cfg, hud::demo_snapshot(), dt_ms, now);
        return;
    }
    ts::drain_events(events);
    hud::feed(g_hud, g_cfg, events, now);
    hud::draw(g_hud, g_cfg, *ts::snapshot(), dt_ms, now);
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
    menu::Host host{g_cfg, g_ini, true, g_hud};
    menu::draw(host);
    if (!host.open && g_menu_open.load()) PostMessageW(g_hwnd, WM_YAP_TOGGLE, 0, 0);  // window's [x]
}

// Keep our window aligned with the game and sized to its client area, and only
// visible while the game (or our own menu) is in the foreground: TOPMOST would
// otherwise float over every other app when the player tabs out.
void track_game_window() {
    static bool shown = true;
    HWND game = FindWindowW(kGameWindowClass, nullptr);
    HWND fg = GetForegroundWindow();
    const bool visible = game && !IsIconic(game) && (fg == game || (fg == g_hwnd && g_menu_open.load()));
    if (visible != shown) {
        ShowWindow(g_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        shown = visible;
        g_visible = visible;
    }
    if (!visible) return;
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
    const ULONGLONG now = GetTickCount64();
    const float dt_ms = g_last_frame ? static_cast<float>(std::min<ULONGLONG>(now - g_last_frame, 250)) : 16.f;
    g_last_frame = now;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_hud(dt_ms, now);
    draw_notice();
    if (g_menu_open.load()) draw_menu();
    ImGui::Render();
    const float clear[4] = {0.f, 0.f, 0.f, 0.f};  // alpha 0 = game shows through
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

    // Layered + transparent + topmost + no-activate tool window: draws over the
    // game, starts click-through, never steals focus until the menu opens.
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"YapNotifier", WS_POPUP, r.left, r.top, static_cast<int>(w), static_cast<int>(h),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        log::error("overlay: CreateWindowEx failed ({})", GetLastError());
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    const MARGINS glass{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hwnd, &glass);
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
    SetDarkPastelImGuiStyle();
    load_font();
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
        if (g_visible.load()) render_frame();
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
