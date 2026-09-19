# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

YapNotifier: a TeamSpeak 3 "who's talking" ImGui overlay for FiveM. Two shipped artifacts from one CMake tree:

- `YapNotifier.asi` — x64 DLL loaded by FiveM's ASI loader from its `plugins/` folder; hooks the game's swapchain and draws the overlay.
- `YapNotifier.ts3_plugin` — TeamSpeak 3 client plugin (`yapnotifier_ts3_win64.dll` + `package.ini` zipped); pushes talk state to the `.asi` over localhost UDP.

Design spec: `docs/superpowers/specs/2026-09-19-yapnotifier-design.md`.

## Build

Requires MSVC (VS 2022 x64) and CMake >= 3.24 (`winget install Kitware.CMake`). Deps (MinHook, Dear ImGui, TS3 plugin SDK headers) are fetched by CMake; no submodules.

```
cmake -S . -B build -A x64 [-DYAPNOTIFIER_DEPLOY_DIR="C:/path/to/FiveM/plugins"]
cmake --build build --config Release
ctest --test-dir build -C Release        # runs test_parser
```

Outputs: `build/Release/YapNotifier.asi`, `build/YapNotifier.ts3_plugin`. With `YAPNOTIFIER_DEPLOY_DIR` set the `.asi` is copied there after every build. Everything compiles with `/W4 /permissive-`; keep it warning-free.

`YapNotifier.rc` carries the `FX_ASI_BUILD` resources FiveM requires (one per game build; add a line when a new build ships or the plugin silently refuses to load) plus version info. Bump `FILEVERSION` there, `ts3plugin_version()` in `ts3plugin/plugin.cpp`, and `ts3plugin/package.ini` together.

Only the datagram parser has a host-side test; hooks, overlay and the TS3 plugin can only be verified in-game / in-client. Runtime log: `<plugins>/YapNotifier.log`; settings: `<plugins>/YapNotifier.ini`. In-game: **INSERT** opens the config menu, **END** ejects the plugin (dev hot-reload).

## Architecture

Data flows one way: TeamSpeak -> plugin -> UDP -> `.asi` listener thread -> atomic snapshot -> render thread.

- **Wire format** (`shared/yap_protocol.h`, included by both sides): one UDP datagram to `127.0.0.1:25640` carrying the *complete* talking list (`YAP1\n` then `clid\tnickname\n` lines), sent on every change and as a 1 s heartbeat. No handshake — the `.asi` treats 3 s of silence as "plugin gone". Change the format in the header, the plugin's `send_state_locked`, `parse_datagram`, and `tests/test_parser.cpp` together.
- **TS3 plugin** (`ts3plugin/plugin.cpp`): C++ over the C SDK; exports `ts3plugin_*`. `onTalkStatusChangeEvent` maintains a `(server, clid) -> nickname` map; move/kick/disconnect events evict clients that will never send "not talking". Excludes the own client.
- **.asi threads:**
  - *Init/eject* (`src/main.cpp`): spawned from `DllMain` (never work under the loader lock). Installs hooks, starts the listener, polls END, tears down in reverse order.
  - *Render* (`src/hooks.cpp` -> `src/overlay.cpp`): the game's thread, entered via MinHook detours on `IDXGISwapChain::Present` (slot 8) / `::ResizeBuffers` (slot 13). Vtable addresses come from a throwaway swapchain on a hidden window — every swapchain in the process shares dxgi's vtable, so no pattern scanning. `overlay` lazily inits ImGui from the real swapchain, subclasses the game window's WndProc, and must release the backbuffer RTV in `on_resize()` *before* the original ResizeBuffers runs.
  - *Listener* (`src/teamspeak.cpp`): binds the UDP port, parses datagrams, publishes immutable `Snapshot`s through one `std::atomic<std::shared_ptr<const Snapshot>>`. The render thread only ever calls `ts::snapshot()`.

Failure policy everywhere: log and go dormant, never crash the game or the TS client.
