# YapNotifier — Design

TeamSpeak 3 "who's talking" overlay for FiveM, shipped as an x64 `.asi` plugin loaded by FiveM's ASI loader.

## Scope

**In:** ImGui overlay listing currently-talking TeamSpeak users, fed by the TS3 ClientQuery plugin. Config menu on a hotkey. File logging. Clean dev-time eject.

**Out (this pass):** 3D positioning above player heads. Any TS ↔ game-player mapping.

## Build

- CMake ≥ 3.24, MSVC, x64, C++20 (needed for `std::atomic<std::shared_ptr>`).
- Deps via FetchContent: MinHook (own CMakeLists), Dear ImGui (custom static-lib target: core + `imgui_impl_win32` + `imgui_impl_dx11`).
- Output `YapNotifier.asi` (`SUFFIX ".asi"`). Optional `YAPNOTIFIER_DEPLOY_DIR` cache var copies the build to FiveM's `plugins/` post-build.

## Layout

```
CMakeLists.txt
src/
  main.cpp          DllMain, init thread, END-key eject
  log.h/.cpp        file logger → <plugins>/YapNotifier.log
  config.h/.cpp     INI load/save via GetPrivateProfileString / WritePrivateProfileString
  hooks.h/.cpp      MinHook init, dummy-swapchain vtable lookup, Present / ResizeBuffers / WndProc
  overlay.h/.cpp    ImGui lifecycle + drawing
  teamspeak.h/.cpp  ClientQuery client thread + snapshot publisher (stubbed protocol)
```

No `include/` — nothing outside the DLL consumes these headers.

## Lifecycle

1. `DllMain(DLL_PROCESS_ATTACH)`: `DisableThreadLibraryCalls`, spawn init thread, return. Never block under loader lock.
2. Init thread: open log → load config → `MH_Initialize` → resolve vtable → create+enable hooks → start TS thread → loop polling END key for eject.
3. Eject: `MH_DisableHook(MH_ALL_HOOKS)` → sleep ~100 ms (let in-flight Present leave the trampoline) → restore WndProc → ImGui shutdown → stop TS thread → `MH_Uninitialize` → `FreeLibraryAndExitThread`.

## Hooking

**Vtable acquisition — dummy swapchain.** Create a throwaway D3D11 device + windowed 1×1 swapchain on the game HWND, read `vtable[8]` (Present) and `vtable[13]` (ResizeBuffers), release, hook those addresses. All `IDXGISwapChain*` instances from the same `dxgi.dll` share the vtable, so the game's swapchain hits our detour. Chains correctly with other overlays (Steam/Discord/FiveM) already hooked in.

Rejected: pattern scanning `dxgi.dll`/`GTA5.exe` (rots on every Windows/game update); locating the game's swapchain object (same fragility, and we don't need the object — Present hands us `this`).

**Present hook.** First call: `GetDevice` → `ID3D11Device`, `GetImmediateContext`, `GetBuffer(0)` → RTV, init ImGui Win32+DX11 backends, `SetWindowLongPtr(GWLP_WNDPROC)` on `GetDesc().OutputWindow`. Every call: if RTV missing, recreate; NewFrame → draw → Render → original.

**ResizeBuffers hook.** Release RTV + `ImGui_ImplDX11_InvalidateDeviceObjects` *before* calling original; Present recreates lazily. Holding the RTV across ResizeBuffers → `DXGI_ERROR_INVALID_CALL`.

**WndProc.** Forward to `ImGui_ImplWin32_WndProcHandler`. While the menu is open, swallow mouse/keyboard messages so the game ignores UI input. Menu hotkey (default INSERT) polled with `GetAsyncKeyState` in Present.

## TeamSpeak client

- Own thread. Connects to `127.0.0.1:25639` (configurable), telnet-style ClientQuery.
- Loop: connect → `auth apikey=…` → `clientnotifyregister schandlerid=0 event=any` (or targeted talk-status events) → read lines → update owned `std::map<uint16_t clid, Client{nickname, talking, channel}>`.
- On every change: build a new immutable `Snapshot` and store into `std::atomic<std::shared_ptr<const Snapshot>>`. Render thread does one `load()` per frame. No mutex, no double buffer.
- Any failure (TS not running, ClientQuery disabled, bad key, socket drop): publish empty snapshot, log, back off, retry forever. Never throws out of the thread.
- This pass: threading, socket lifecycle, snapshot plumbing, and a line-parser entry point are real; protocol parsing is stubbed with marked TODOs.

## Overlay

- Panel listing talking users (name + speaking indicator). Configurable position, opacity, scale.
- Config menu (INSERT): edit API key/host/port, position, opacity, scale; Save writes INI.
- Nothing to show → draw nothing.

## Config

`<plugins>/YapNotifier.ini`: `api_key`, `host`, `port`, `menu_key`, `pos_x`, `pos_y`, `opacity`, `scale`. Missing file → defaults, overlay idle until a key is set.

## Failure policy

Every subsystem fails closed: hook resolution failure → log and stay dormant; ImGui init failure → pass-through Present; TS failure → empty overlay + retry. No path may crash the game.

## Testing

Game-in-the-loop for hooks/overlay. One host-side `test_parser.cpp` for ClientQuery line parsing (`\s`, `\p`, `\/` unescaping, `key=value|key=value` records) once the protocol is filled in.
