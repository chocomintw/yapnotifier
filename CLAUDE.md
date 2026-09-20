# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

YapNotifier: a TeamSpeak 3 channel/talk-state ImGui overlay for FiveM. Two shipped artifacts from one CMake tree:

- `YapNotifier.asi` — x64 DLL loaded by FiveM's ASI loader from its `plugins/` folder; draws the overlay in its own transparent topmost window (no game hooks).
- `YapNotifier.ts3_plugin` — TeamSpeak 3 client plugin (`yapnotifier_ts3_win64.dll` + `package.ini` zipped); pushes the channel roster, per-client state and events to the `.asi` over localhost UDP.

Design specs: `docs/superpowers/specs/2026-09-19-yapnotifier-design.md` (original, hook-era sections are obsolete) and `docs/superpowers/specs/2026-09-20-feature-port-design.md` (protocol v2, config, HUD).

## Build

Requires MSVC (VS 2022 x64) and CMake >= 3.24 (`winget install Kitware.CMake`). Deps (Dear ImGui, TS3 plugin SDK headers) are fetched by CMake; no submodules.

```
cmake -S . -B build -A x64 [-DYAPNOTIFIER_DEPLOY_DIR="C:/path/to/FiveM/plugins"]
cmake --build build --config Release
ctest --test-dir build -C Release        # runs test_parser
```

Outputs: `build/Release/YapNotifier.asi`, `build/YapNotifier.ts3_plugin`. With `YAPNOTIFIER_DEPLOY_DIR` set the `.asi` is copied there after every build. Everything compiles with `/W4 /permissive-`; keep it warning-free.

`YapNotifier.rc` carries the `FX_ASI_BUILD` resources FiveM requires (one per game build; add a line when a new build ships or the plugin silently refuses to load), the embedded Quicksand font (`YAP_FONT` RCDATA) and version info.

**Releasing:** bump `YAP_VERSION` / `YAP_VERSION_NUM` in `shared/version.h` (feeds the `.rc`, the TS3 plugin, and the updater) and `Version` in `ts3plugin/package.ini` (CI fails if they differ), then push a `v<version>` tag. The workflow builds, tests, and publishes a GitHub Release with `YapNotifier.asi` + `YapNotifier.ts3_plugin`; installed copies self-update from it.

`test_parser` covers the pure logic (datagram parser, config colour/enum parsing, template formatting, toast queue, roster rules, updater helpers); the overlay window and the TS3 plugin can only be verified in-game / in-client. Runtime log: `<plugins>/YapNotifier.log`; settings: `<plugins>/YapNotifier.ini`; profiles: `<plugins>/YapNotifier.profiles/*.ini`. In-game: **INSERT** opens the config menu (tabs: Status, Layout, Roster, Indicators, Notifications, Chat, Users, Channel, Profiles). **Demo mode** (Status tab) renders a fake roster + scripted events without TeamSpeak. There is no eject key; the plugin lives for the whole process.

## Architecture

Data flows one way: TeamSpeak -> plugin -> UDP -> `.asi` listener thread -> atomic snapshot + event queue -> render thread.

- **Wire format v2** (`shared/yap_protocol.h`, included by both sides): one UDP datagram to `127.0.0.1:25640` carrying the *complete* state of the active TS tab: `YAP2\n`, then tab-separated `S` (connection + server name), `C` (channel id/parent/name), `U` (one per client in the own channel: clid, flags bitmask, unique id, contact nickname, display name) and `E` (one-shot events: join/leave/switch/conn/whisper/chat) lines. The last field of a line is the remainder of the line, so free text goes last. Sent on every change and as a 1 s heartbeat (heartbeats carry no `E`); the `.asi` treats 3 s of silence as "plugin gone". `YAP1` (talk-only) is still accepted and flagged `legacy` so an auto-updated `.asi` keeps working with an old plugin. Change the format in the header, the plugin's `send_locked`, `parse_datagram`, and `tests/test_parser.cpp` together.
- **TS3 plugin** (`ts3plugin/plugin.cpp`): C++ over the C SDK; exports `ts3plugin_*`. Mirrors only the active tab (`currentServerConnectionChanged`). Every roster-affecting callback runs `resync_locked()` (full re-read of the own channel via `getChannelClientList` + client properties); talk/whisper state lives in two sets and is OR'd in at send time. Move callbacks emit join/leave/switch events with channel names; text/poke callbacks emit chat events (truncated to `kMaxChatChars`, always `return 0` so TS still shows them). Friends come from the client's `settings.db` via `ts_contacts.cpp` / `sqlite_read.cpp` (MIT, copied from TeamSpeak3-Reshade-overlay, see `assets/LICENSE-tsro.txt`).
- **.asi threads:**
  - *Init* (`src/main.cpp`): spawned from `DllMain` (never work under the loader lock). Loads the INI, starts the overlay and the listener, runs the updater, then polls the menu hotkey forever.
  - *UI* (`src/overlay.cpp`): owns a `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST` popup window tracked to the game's client rect every frame, a D3D11 device and a DirectComposition swapchain (premultiplied alpha) and the ImGui context; ~60 fps loop. Click-through is purely `WS_EX_TRANSPARENT`, toggled with the menu; while the menu is open the game's raw mouse registration is suspended (camera lock) and restored on close. No hooks, no WndProc subclassing — FiveM's anti-cheat kills those. Per frame: drain `ts::drain_events` into `hud::feed`, then `hud::draw` (menu closed content) and `menu::draw` (when open).
  - *Listener* (`src/teamspeak.cpp`): binds the UDP port, parses datagrams, publishes immutable `Snapshot`s through one `std::atomic<std::shared_ptr<const Snapshot>>` and pushes `Event`s into a mutex-guarded queue (cap 64). The render thread only ever calls `ts::snapshot()` / `ts::drain_events()`.
- **HUD** (`src/hud.cpp`): draws into ImGui's background draw list (never hit-testable): channel title, roster rows (leading/trailing vector icons from `src/icons.cpp`, friend tag, ellipsised names, "+N more"), toasts (fade in/hold/out, merge duplicates, quiet window after connect) and the chat feed. Anchoring is 9-point (`Anchor`) + px offsets. Pure rules live in `src/roster.cpp` (visibility, sort, state->colour/icon priority, speaking envelope) and `src/notify.cpp` (template formatting, toast queue, chat filtering) so `test_parser` can pin them.
- **Config** (`src/config.cpp`): flat `[YapNotifier]` INI via the Win32 profile API; one `visit()` field list drives load and save. Colours are `#RRGGBBAA` (`0` = unset). Per-user overrides live in `[user:<unique id>]` sections, per-channel in `[channel:<id>]`. Profiles are plain copies of the INI. `demo` is never saved.
- **Menu** (`src/menu.cpp`): tabbed ImGui window editing the live `Config`; edits apply immediately, Save writes the INI. Font file/size changes apply on next launch.
- **Updater** (`src/update.cpp`, runs on the init thread): `GET api.github.com/repos/chocomintw/yapnotifier/releases/latest` via WinHTTP, compares `tag_name` to `YAP_VERSION`, downloads the `YapNotifier.asi` asset, verifies it against the asset `digest` (SHA-256 via BCrypt), then renames the running `.asi` to `.asi.old` and drops the new file in place — Windows allows renaming a mapped DLL, so the swap is immediate and takes effect on the next FiveM start. `.asi.old` is deleted on the following launch. `auto_update=0` in the INI checks but does not install. Status surfaces through `update::notice()` as a 20 s banner and a line in the menu.

Failure policy everywhere: log and go dormant, never crash the game or the TS client.
