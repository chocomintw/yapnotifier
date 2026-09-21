# YapNotifier

A TeamSpeak 3 overlay for FiveM. Shows who is in your channel, who is talking, muted or away, plus join/leave/whisper notifications and a chat feed — drawn in a transparent window on top of the game, with no game hooks.

Two parts, one release:

| File | Goes where | Does |
|------|-----------|------|
| `YapNotifier.asi` | FiveM `plugins/` folder | Draws the overlay and the config menu |
| `YapNotifier.ts3_plugin` | TeamSpeak 3 client | Sends your channel state to the overlay over localhost UDP (port 25640) |

## Install

1. Download both files from the [latest release](https://github.com/chocomintw/yapnotifier/releases/latest).
2. **Overlay:** copy `YapNotifier.asi` into FiveM's `plugins` folder: `%LOCALAPPDATA%\FiveM\FiveM.app\plugins` (create `plugins` if it does not exist).
3. **TeamSpeak plugin:** double-click `YapNotifier.ts3_plugin`. TeamSpeak installs it and asks to enable it (or enable it under *Tools → Options → Addons*).
4. Start FiveM. Join a channel in TeamSpeak and the roster appears.

The overlay checks GitHub for a new version on every launch and installs it for the next start (`auto_update=0` in the INI turns installing off). The TeamSpeak plugin is not auto-updated; grab it from the release when the overlay tells you it is outdated.

## Use

- **INSERT** opens the config menu (rebindable via `menu_key` in the INI, a Windows virtual-key code). While it is open the game's mouse is released; close it with INSERT again or the menu's ×.
- **Status tab → Demo mode** renders a fake roster and scripted events so you can lay everything out without TeamSpeak running.
- Tabs: Status, Layout (anchor, offsets, scale, font), Roster (visibility, sorting, colours), Indicators (per-state icons/colours), Notifications, Chat, Users (per-person overrides), Channel (per-channel overrides), Profiles (save/load whole configs).

Edits apply immediately; **Save** writes them. Only changing the font file needs a restart.

Files, next to the `.asi`:

- `YapNotifier.ini` — settings (`[YapNotifier]`, plus `[user:<unique id>]` and `[channel:<id>]` overrides)
- `YapNotifier.profiles\*.ini` — saved profiles
- `YapNotifier.log` — runtime log; first place to look when something is off

## Notes

- Only the **active** TeamSpeak tab is mirrored.
- The overlay hides when FiveM is not the foreground window.
- It is a separate window, not a swap-chain hook: game-capture tools (Steam F12, ShadowPlay, OBS *Game Capture*, Discord game capture) do not record it; screen/display capture does.
- If the roster shows *TS3 plugin outdated*, update `YapNotifier.ts3_plugin`.
- If nothing shows up: check `YapNotifier.log`, confirm the FiveM build is one the plugin declares (`YapNotifier.rc`), and that nothing else is bound to UDP 25640.

## Build from source

Requires Visual Studio 2022 (x64 MSVC) and CMake ≥ 3.24. Dependencies (RmlUi, FreeType, LunaSVG, TS3 plugin SDK) are fetched by CMake on first configure.

```
cmake -S . -B build -A x64 [-DYAPNOTIFIER_DEPLOY_DIR="C:/path/to/FiveM/plugins"]
cmake --build build --config Release
ctest --test-dir build -C Release
```

Outputs `build/Release/YapNotifier.asi` and `build/YapNotifier.ts3_plugin`. With `YAPNOTIFIER_DEPLOY_DIR` set, the `.asi` is copied there after every build.

**Releasing:** bump `YAP_VERSION` / `YAP_VERSION_NUM` in `shared/version.h` and `Version` in `ts3plugin/package.ini`, push a `v<version>` tag. CI builds, tests and publishes the GitHub Release.

See `CLAUDE.md` for the architecture and `docs/superpowers/specs/` for the design documents.

## Credits

- Contact/friend lookup from the TeamSpeak `settings.db` is adapted from TeamSpeak3-Reshade-overlay (MIT, `assets/LICENSE-tsro.txt`).
- UI font: [Inter](https://rsms.me/inter/) (SIL OFL, `assets/OFL-Inter.txt`).
- Rendering: [RmlUi](https://github.com/mikke89/RmlUi).
