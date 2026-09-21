#include "overlay.h"

#include "colorpicker.h"
#include "hud.h"
#include "log.h"
#include "menu.h"
#include "teamspeak.h"
#include "ui_files.h"
#include "update.h"

#include <Windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>

#include <RmlUi/Core.h>
#include <RmlUi_Platform_Win32.h>
#include <RmlUi_Renderer_DX11.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <optional>
#include <thread>
#include <vector>

namespace {
using namespace yap;

// Per-pixel alpha over the game via a DirectComposition swapchain on a layered window
// whose own (never painted) surface is made transparent by extending the DWM frame.
// LWA_ALPHA 255 means hit-testing is whole-window, so click-through is purely
// WS_EX_TRANSPARENT, toggled with the menu. (LWA_COLORKEY was visually right but user32
// hit-tested the never-painted GDI bitmap, so every click fell through; and
// WS_EX_TRANSPARENT only passes input through when the window is also WS_EX_LAYERED.)
constexpr UINT WM_YAP_TOGGLE = WM_APP + 1;

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

// RmlUi: one context holding the HUD document (always shown) and the menu document.
std::optional<SystemInterface_Win32> g_system;
std::optional<RenderInterface_DX11> g_renderer;
TextInputMethodEditor_Win32 g_ime;
Rml::Context* g_rml = nullptr;
std::optional<menu::Host> g_menu_host;

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

// A DLL from System32 by absolute path. ReShade installs itself as a proxy dxgi.dll and ENB
// as a proxy d3d11.dll, already loaded under those names by the time we run, so the plain
// imports would route our device through them; our window has nothing to do with the game's
// rendering, so ask Windows for the real implementations (borrowed from tsro).
HMODULE system_library(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    return LoadLibraryW((std::wstring(dir, n) + L"\\" + name).c_str());
}

using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                             UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

bool create_device_unguarded(HWND hwnd, UINT w, UINT h) {
    const HMODULE real_d3d11 = system_library(L"d3d11.dll"), real_dxgi = system_library(L"dxgi.dll");
    const auto create_d3d11 =
        real_d3d11 ? reinterpret_cast<D3D11CreateDeviceFn>(GetProcAddress(real_d3d11, "D3D11CreateDevice")) : nullptr;
    const auto create_factory =
        real_dxgi ? reinterpret_cast<CreateFactory2Fn>(GetProcAddress(real_dxgi, "CreateDXGIFactory2")) : nullptr;
    if (!create_d3d11 || !create_factory) {
        log::error("overlay: the system d3d11.dll/dxgi.dll could not be loaded");
        return false;
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = create_d3d11(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                              D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx);
    IDXGIDevice* dxgi = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(hr)) hr = g_device->QueryInterface(IID_PPV_ARGS(&dxgi));
    // The factory this device was made on: one consistent object graph. An independent
    // CreateDXGIFactory2 is what faulted inside ReShade's dxgi for tsro.
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    release(adapter);
    if (FAILED(hr)) hr = create_factory(0, IID_PPV_ARGS(&factory));
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

// Other people's graphics DLLs live in this process too and a fault in theirs kills the game
// just the same; an overlay is not worth that, so a fault here just switches us off.
bool create_device(HWND hwnd, UINT w, UINT h) {
    __try {
        return create_device_unguarded(hwnd, w, h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::error("overlay: graphics setup faulted; overlay disabled for this session, game unaffected");
        return false;
    }
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
// The game's window: the largest visible top-level window of *this process*. Not a class
// lookup (FindWindowW("grcWindow") is process-wide: on a restart it found the previous
// instance's dying/minimised window and we followed that off-screen).
struct GameWindowSearch {
    DWORD pid = 0;
    HWND best = nullptr;
    long best_area = 0;
};

BOOL CALLBACK pick_game_window(HWND hwnd, LPARAM param) {
    auto* s = reinterpret_cast<GameWindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid || hwnd == g_hwnd || !IsWindowVisible(hwnd)) return TRUE;
    RECT c{};
    if (!GetClientRect(hwnd, &c)) return TRUE;
    const long area = (c.right - c.left) * (c.bottom - c.top);
    if (area > s->best_area) {
        s->best_area = area;
        s->best = hwnd;
    }
    return TRUE;
}

HWND game_window() {
    GameWindowSearch s;
    s.pid = GetCurrentProcessId();
    EnumWindows(pick_game_window, reinterpret_cast<LPARAM>(&s));
    return s.best;
}

// Screen rect of the game's client area, so our window covers exactly it.
bool game_client_rect(RECT& out) {
    HWND game = game_window();
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
    switch (m) {
        case WM_YAP_TOGGLE: {
            bool open = !g_menu_open.load();
            g_menu_open = open;
            set_click_through(!open);
            if (g_menu_host) {
                g_menu_host->open = open;
                menu::show(open);
            }
            if (open) {
                SetForegroundWindow(h);
                SetFocus(h);
                suspend_game_mouse();
            } else {
                restore_game_mouse();
                if (HWND game = game_window()) SetForegroundWindow(game);
            }
            return 0;
        }
        case WM_DESTROY:
            restore_game_mouse();
            PostQuitMessage(0);
            return 0;
    }
    // Input only reaches us while the menu is open (closed, the window is click-through).
    if (g_rml && !RmlWin32::WindowProcedure(g_rml, g_ime, h, m, w, l)) return 0;
    return DefWindowProcW(h, m, w, l);
}

// `font_file` (relative to the plugins dir) if set, else Inter from the RCDATA resource in
// YapNotifier.rc. Both register as family "Yap", which the RCSS uses; weight Auto loads every
// named instance of a variable font (Inter: 100..900) so font-weight 500/600 resolve.
void load_font() {
    if (!g_cfg.font_file.empty()) {
        std::filesystem::path p = g_cfg.font_file;
        if (p.is_relative()) p = std::filesystem::path(g_ini).parent_path() / p;
        if (Rml::LoadFontFace(p.string(), "Yap", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto)) return;
        log::error("overlay: could not load font {}, using Inter", p.string());
    }
    auto blob = ui_files::resource(L"YAP_FONT");
    if (blob.empty() || !Rml::LoadFontFace(blob, "Yap", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto))
        log::error("overlay: font resource missing, text will not render");
}

// --- content -----------------------------------------------------------------
void sync_hud(float dt_ms, ULONGLONG now) {
    static std::vector<ts::Event> events;
    events.clear();
    // The updater's banner: 20 s after it first appears.
    static ULONGLONG notice_seen = 0;
    auto notice = update::notice();
    if (!notice->empty() && !notice_seen) notice_seen = now;
    const std::string banner = notice_seen && now - notice_seen <= 20000 ? *notice : std::string();

    if (g_cfg.demo) {
        hud::demo_tick(g_hud, g_cfg, now);
        std::vector<ts::Event> dropped;
        ts::drain_events(dropped);  // keep the real queue from piling up meanwhile
        hud::sync(g_hud, g_cfg, hud::demo_snapshot(), dt_ms, now, banner);
        return;
    }
    ts::drain_events(events);
    hud::feed(g_hud, g_cfg, events, now);
    hud::sync(g_hud, g_cfg, *ts::snapshot(), dt_ms, now, banner);
}

// Keep our window aligned with the game and sized to its client area, and only
// visible while the game (or our own menu) is in the foreground: TOPMOST would
// otherwise float over every other app when the player tabs out.
void track_game_window() {
    static bool shown = true;
    HWND game = game_window();
    // "Foreground" = any window of this process: the game, our overlay, or whatever FiveM
    // has up. During the SetForegroundWindow handoff between our window and the game
    // (menu open/close) GetForegroundWindow() is briefly NULL, so a foreign foreground
    // only hides us once it has outlasted that gap; showing is immediate.
    static ULONGLONG foreign_since = 0;
    DWORD fg_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    const bool ours = fg_pid == GetCurrentProcessId();
    const ULONGLONG now = GetTickCount64();
    if (ours) foreign_since = 0;
    else if (!foreign_since) foreign_since = now;
    const bool visible = game && !IsIconic(game) && (ours || now - foreign_since < 250);
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
            g_renderer->SetViewport(static_cast<int>(w), static_cast<int>(h));
            g_rml->SetDimensions({static_cast<int>(w), static_cast<int>(h)});
        }
    }
}

void render_frame() {
    if (!g_rtv) return;
    if (g_menu_open.load()) ClipCursor(nullptr);
    const ULONGLONG now = GetTickCount64();
    const float dt_ms = g_last_frame ? static_cast<float>(std::min<ULONGLONG>(now - g_last_frame, 250)) : 16.f;
    g_last_frame = now;

    g_rml->SetDensityIndependentPixelRatio(g_cfg.scale);  // every RCSS size is in dp
    sync_hud(dt_ms, now);
    if (g_menu_open.load()) {
        menu::sync(*g_menu_host);
        if (!g_menu_host->open) PostMessageW(g_hwnd, WM_YAP_TOGGLE, 0, 0);  // window's [x]
    }
    g_rml->Update();

    // Alpha 0 = game shows through; the backend composites its (transparent-cleared)
    // layer onto this with premultiplied ONE/INV_SRC_ALPHA, so untouched pixels stay 0.
    const float clear[4] = {0.f, 0.f, 0.f, 0.f};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    g_renderer->BeginFrame();
    g_rml->Render();
    g_renderer->EndFrame(g_rtv);
    g_swapchain->Present(0, 0);  // no vsync: never contend with the game's swapchain
}

bool init_rml(UINT w, UINT h) {
    g_system.emplace();
    g_system->SetWindow(g_hwnd);
    g_renderer.emplace(g_device);
    g_renderer->SetViewport(static_cast<int>(w), static_cast<int>(h));
    Rml::SetSystemInterface(&*g_system);
    Rml::SetRenderInterface(&*g_renderer);
    Rml::SetFileInterface(&ui_files::instance());
    if (!Rml::Initialise()) {
        log::error("overlay: Rml::Initialise failed");
        return false;
    }
    Rml::SetTextInputHandler(&g_ime);
    colorpicker::register_element();
    load_font();
    g_rml = Rml::CreateContext("yap", {static_cast<int>(w), static_cast<int>(h)});
    if (!g_rml) {
        log::error("overlay: Rml::CreateContext failed");
        return false;
    }
    g_rml->SetDensityIndependentPixelRatio(g_cfg.scale);
    if (!hud::init(*g_rml)) return false;
    g_menu_host.emplace(menu::Host{g_cfg, g_ini, false, g_hud});
    return menu::init(*g_menu_host, *g_rml);
}

void shutdown_rml() {
    if (Rml::GetTextInputHandler() == &g_ime) Rml::SetTextInputHandler(nullptr);
    Rml::Shutdown();
    g_rml = nullptr;
    g_menu_host.reset();
    g_renderer.reset();
    g_system.reset();
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

    // The game may not have a window yet: wait for one (up to 60 s) rather than sizing
    // ourselves to the whole screen and drawing the HUD relative to the wrong rect.
    RECT r{};
    for (int i = 0; i < 600 && !g_stop.load(); ++i) {
        if (game_client_rect(r)) break;
        Sleep(100);
    }
    if (r.right <= r.left) {
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

    if (!create_device(g_hwnd, w, h) || !init_rml(w, h)) {
        shutdown_rml();
        destroy_device();
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    g_ready = true;
    log::info("overlay: ready (own {}x{} window, RmlUi {}, no game hooks)", w, h, Rml::GetVersion());

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

    shutdown_rml();
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
