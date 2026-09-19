# YapNotifier — Design

TeamSpeak 3 "who's talking" overlay for FiveM. Two artifacts: an x64 `.asi` plugin loaded by FiveM's ASI loader, and a TeamSpeak 3 client plugin (`.ts3_plugin`) that feeds it.

## Scope

**In:** ImGui overlay listing currently-talking TeamSpeak users. TS3 plugin as the data source. Config menu on a hotkey. File logging. Clean dev-time eject.

**Out (this pass):** 3D positioning above player heads. Any TS <-> game-player mapping. ClientQuery (replaced by the plugin, 2026-09-19). Linux/macOS plugin builds. Code signing.

## Build

- CMake >= 3.24, MSVC, x64, C++20 (needed for `std::atomic<std::shared_ptr>`).
- Deps via FetchContent: MinHook, Dear ImGui (custom static-lib target), TS3 plugin SDK headers (pinned commit, API 26).
- Static CRT on every binary.
- Targets: `YapNotifier.asi`; `yapnotifier_ts3_win64.dll` zipped with `package.ini` into `YapNotifier.ts3_plugin`; `test_parser` (CTest).
- `YapNotifier.rc` carries the `FX_ASI_BUILD` resources FiveM requires (one per game build) and version info.
- Optional `YAPNOTIFIER_DEPLOY_DIR` copies the `.asi` to FiveM's `plugins/` post-build.

## Layout

```
CMakeLists.txt
YapNotifier.rc
shared/yap_protocol.h   wire format + port, included by both sides
src/                    .asi
  main.cpp              DllMain, init thread, END-key eject
  log.h/.cpp            file logger -> <plugins>/YapNotifier.log
  config.h/.cpp         INI via GetPrivateProfileString / WritePrivateProfileString
  hooks.h/.cpp          MinHook init, dummy-swapchain vtable lookup, Present / ResizeBuffers
  overlay.h/.cpp        ImGui lifecycle, WndProc subclass, drawing
  teamspeak.h/.cpp      UDP listener + datagram parser + snapshot publisher
ts3plugin/
  plugin.cpp            TS3 plugin: talk events -> UDP datagrams
  package.ini           .ts3_plugin manifest
tests/test_parser.cpp   datagram parser check
```

## Wire protocol (plugin -> .asi)

UDP to `127.0.0.1:25640`. Each datagram is the **complete** current talking list:

```
YAP1\n
<clid>\t<nickname>\n   (zero or more)
```

Sent on every change and once per second as a heartbeat. The `.asi` treats >3 s of silence as "plugin gone" and clears the overlay. No handshake, no auth (loopback only; payload is public voice-server state). Chosen over named pipes / shared memory because it has no connection lifecycle on either side.

## TS3 plugin

Exports the required `ts3plugin_*` symbols (API 26). `onTalkStatusChangeEvent` adds/removes `(server, clid)` in a mutex-protected map and resolves the nickname via `getClientVariableAsString(CLIENT_NICKNAME)`. Own client is excluded. Move/kick events with `newChannelID == 0` and `STATUS_DISCONNECTED` remove clients that will never emit "not talking". A heartbeat thread resends state every 1 s. `shutdown()` sends an empty list so the overlay clears immediately.

## .asi lifecycle

1. `DllMain(DLL_PROCESS_ATTACH)`: `DisableThreadLibraryCalls`, spawn init thread, return. Never block under loader lock.
2. Init thread: open log -> load config -> `MH_Initialize` -> resolve vtable -> create+enable hooks -> start listener thread -> poll END key for eject.
3. Eject: `MH_DisableHook(MH_ALL_HOOKS)` -> drain in-flight detours (bounded 1 s) -> `MH_Uninitialize` -> restore WndProc + ImGui shutdown -> stop listener -> `FreeLibraryAndExitThread`.

## Hooking

**Vtable acquisition — dummy swapchain.** Create a throwaway D3D11 device + windowed 1x1 swapchain on our own hidden window, read `vtable[8]` (Present) and `vtable[13]` (ResizeBuffers), release, hook those addresses. All `IDXGISwapChain*` instances from the same `dxgi.dll` share the vtable. Chains with other overlays already hooked in.

Rejected: pattern scanning `dxgi.dll`/`GTA5.exe` (rots on every update); locating the game's swapchain object (same fragility; Present hands us `this` anyway).

**Present hook.** First call: `GetDevice`, `GetImmediateContext`, `GetBuffer(0)` -> RTV, init ImGui Win32+DX11, subclass `GetDesc().OutputWindow`'s WndProc. Every call: recreate RTV if missing; NewFrame -> draw -> Render -> original. `DXGI_PRESENT_TEST` calls skip drawing.

**ResizeBuffers hook.** Release RTV + `ImGui_ImplDX11_InvalidateDeviceObjects` *before* the original; Present recreates lazily.

**WndProc.** Always forwards to `ImGui_ImplWin32_WndProcHandler`; while the menu is open, swallows mouse/keyboard/`WM_INPUT`. Menu hotkey (default INSERT) polled with `GetAsyncKeyState` in Present.

## Listener (.asi)

Own thread. Binds `127.0.0.1:<port>`; `select` with 500 ms timeout so stop and port changes are responsive. Each datagram -> `parse_datagram` -> new immutable `Snapshot{connected, talking[]}` stored in `std::atomic<std::shared_ptr<const Snapshot>>`; render thread does one `load()` per frame. Bind failure -> log, retry every 2 s.

## Overlay

Panel listing talking users (green dot + name); configurable position, opacity, scale. Config menu (INSERT): UDP port, position, opacity, scale, Save. Nothing to show -> draws nothing.

## Config

`<plugins>/YapNotifier.ini`: `port`, `menu_key`, `pos_x`, `pos_y`, `opacity`, `scale`. Missing -> defaults.

## Failure policy

Every subsystem fails closed: hook failure -> dormant; ImGui failure -> pass-through Present; no plugin -> empty overlay. Plugin: socket failure -> `init` returns 1 and TS unloads it. Nothing may crash the game or the TS client.

## Testing

`test_parser` (CTest) covers the datagram parser. Hooks, overlay and the plugin are verified in-game / in-client.
