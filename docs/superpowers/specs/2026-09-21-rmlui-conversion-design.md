# YapNotifier: ImGui -> RmlUi conversion (design)

## Context

The `.asi` overlay draws everything with Dear ImGui: `hud.cpp` paints straight into the background draw list, `menu.cpp` is 9 tabs of immediate-mode widgets, `icons.cpp` is ImDrawList geometry, `theme.h` is an ImGui style table. The user wants the UI on RmlUi (HTML/CSS-like retained documents) so layout, styling and animation move into RML/RCSS and the C++ shrinks to "publish state". This pass is a **conversion**: feature parity, same INI keys, same wire protocol, ImGui deleted at the end. New features come after.

Decisions already made with the user:
- Full parity, ImGui removed (no HUD-only intermediate).
- Icons: RmlUi SVG plugin (LunaSVG). 17 inline SVGs, tinted via RCSS.
- Colours: custom `<colorpicker>` element (SV square + hue bar + alpha bar).
- C++ drives the DOM through **Rml data models** (declarative `data-for` / `data-value` / `data-style-*`).

What stays untouched: `shared/`, `src/teamspeak.*`, `src/roster.*`, `src/notify.*`, `src/config.*`, `src/update.*`, `src/log.*`, `src/main.cpp`, `ts3plugin/`, `tests/test_parser.cpp`. `hud.cpp`'s `feed()` / `demo_snapshot()` / `demo_tick()` stay too (no ImGui in them).

## Design

### Build (`CMakeLists.txt`, `YapNotifier.rc`)
- Drop the `imgui` FetchContent + static lib. Add FetchContent for `freetype` (pin `VER-2-13-3`, `FT_DISABLE_ZLIB/BZIP2/PNG/HARFBUZZ/BROTLI=ON`), `lunasvg` (pin latest 3.x; its plutovg comes along as submodule), and `RmlUi` (pin latest 6.x tag). Use `OVERRIDE_FIND_PACKAGE` (we already require CMake 3.24) so RmlUi's `find_package(Freetype)` / `find_package(lunasvg)` resolve to the fetched targets. RmlUi options: `BUILD_SHARED_LIBS=OFF RMLUI_SAMPLES=OFF RMLUI_LUA_BINDINGS=OFF RMLUI_SVG_PLUGIN=ON RMLUI_FONT_ENGINE=freetype`. Static CRT is inherited from `CMAKE_MSVC_RUNTIME_LIBRARY`.
- New static lib `rmlui_backend` compiling `Backends/RmlUi_Platform_Win32.cpp` + `Backends/RmlUi_Renderer_DX11.cpp` from the RmlUi source dir (like today's `imgui` lib: no `/W4` on third-party code). Mark RmlUi/lunasvg include dirs SYSTEM so `/W4 /permissive-` stays warning-free on our TUs. Links `d3d11 d3dcompiler` (the DX11 backend compiles HLSL at runtime).
- `.rc`: keep `YAP_FONT`; add one `RCDATA` line per UI file under `assets/ui/` (`hud.rml`, `menu.rml`, `theme.rcss`, `icons/*.svg`). Resource name = file name, so the `.asi` stays a single self-updating file.
- `test_parser` unchanged. Add `test_ui` (see Verification).

### `src/ui_files.cpp` (new, ~50 lines) — resource-backed `Rml::FileInterface`
Opens `name` by `FindResourceW(self, name, RT_RCDATA)`; Read/Seek/Tell over the locked blob. Reuses the `GetModuleHandleExW(FROM_ADDRESS)` trick already in `overlay.cpp::load_font`. Both `<link href="theme.rcss">` and `<svg src="icons/mic.svg">` go through it. Also exposes `font_blob()` for the embedded Quicksand so `overlay.cpp` can `Rml::LoadFontFace(span, "Quicksand", ...)` from memory; `cfg.font_file` still loads from disk via `Rml::LoadFontFace(path)` with a fallback to the resource, same policy as today's `load_font()`.

### `src/overlay.cpp` — swap the UI library, keep the window
Keep verbatim: window class/creation, `WS_EX_LAYERED|TRANSPARENT|TOPMOST` handling, `set_click_through`, DirectComposition swapchain, `track_game_window` foreground gating, camera lock (`suspend_game_mouse`/`restore_game_mouse`), `WM_YAP_TOGGLE`, frame pacing, `draw_hud` event plumbing.
Replace:
- Init: `SystemInterface_Win32` + `RenderInterface_DX11` (init with our `g_device`/`g_ctx`), `Rml::SetFileInterface(ui_files)`, `Rml::Initialise()`, `Rml::SVG::Initialise()`, register `<colorpicker>` (`Rml::Factory::RegisterElementInstancer`), load fonts, `CreateContext("yap", {w,h})`, `hud = ctx->LoadDocument("hud.rml")` + `Show()`, `menu = ctx->LoadDocument("menu.rml")` (hidden until toggle). Menu open/close = `menu->Show()/Hide()` inside the `WM_YAP_TOGGLE` branch. `ImGui::GetIO().MouseDrawCursor` goes away; the class cursor (`IDC_ARROW`) + RmlUi's `SetMouseCursor` handle it, so the `WM_SETCURSOR` override is deleted.
- `wndproc`: `RmlWin32::WindowProcedure(ctx, text_input_method_editor, h, m, w, l)` first (it only matters while the menu is open — closed, the window is click-through and gets nothing).
- Frame: `hud::sync(...)`, `menu::sync(...)` (dirty data models), `ctx->Update()`, clear RTV to `{0,0,0,0}`, `renderer.BeginFrame(rtv)` / `ctx->Render()` / `EndFrame()`, `Present`. RmlUi 6 backends output premultiplied alpha, which is exactly what the `DXGI_ALPHA_MODE_PREMULTIPLIED` swapchain expects — verify in step 1 that the DX11 backend's blend state leaves cleared pixels at alpha 0 (transparent over the game).
- Resize: `ctx->SetDimensions` + `renderer.SetViewport` next to the existing `ResizeBuffers`.
- Notice banner (`draw_notice`) becomes a `<div id="notice">` in `hud.rml` bound to a `notice` string in the HUD data model; the 20 s timer stays in C++.

### `assets/ui/hud.rml` + `src/hud.cpp` — HUD as a data model
`hud::State` keeps `env`, `toasts`, `chat`, idle fade, demo fields. `hud::draw(...)` becomes `hud::sync(State&, const Config&, const ts::Snapshot&, float dt_ms, uint64_t now_ms)`: runs the same envelope/toast/idle ticks, then fills a POD view struct registered as data model `hud`:
- `title` (`title_text()` unchanged), `title_color`, `title_icon` (svg path or ""), `roster_alpha`, `legacy` bool, `more` string.
- `rows[]`: `{name, name_color, tag, tag_color, leading[] {svg, color, opacity}, trailing[] {...}, opacity}` built from `roster::visible` + `roster::resolve` exactly as today (`draw_roster` measuring/positioning code disappears). Speaking pulse: `trailing[i].opacity = level * pulse` computed in C++, applied through `data-style-opacity`.
- `toasts[]`: `{prefix, prefix_color, text, count, color, opacity}` from `notify::Queue::alpha`.
- `chat[]`: `{prefix, body, cat}` from `notify::chat_visible`; wrapping is RCSS, prefix colour is a `<span>`.
- `notice`.
Anchoring: the three blocks are `position: absolute`; `sync` writes `left/right/top/bottom` (+ `transform: translate(-50%)` for centre columns/rows) and `width` with `SetProperty` only when the relevant Config fields change (compare against a cached copy). Text shadow/outline -> `font-effect: shadow(...)` / `outline(...)` classes toggled on `<body>`. `max_name_width` -> `max-width; white-space: nowrap; overflow: hidden` (clip instead of "..."; `ponytail:` note, add real ellipsis via `GetFontEngineInterface()->GetStringWidth` if missed). `cfg.scale` -> `ctx->SetDensityIndependentPixelRatio(cfg.scale)` with every RCSS size in `dp`, one call covers it. Font size: `font-size` on `<body>` from `cfg.font_size`.
`DirtyAllVariables()` once per frame (roster <= 24 rows, cheap).

### `assets/ui/menu.rml` + `src/menu.cpp` — menu as a data model over `Config`
`menu::Host` stays. `menu::init(Host&, Rml::Context&)` registers data model `cfg`: plain fields via `Bind(&cfg.x)` (bool/int/float/string map directly); `Color` via `BindFunc` getter/setter using `config::format_color`/`parse_color` (string `#RRGGBBAA`, the picker's value format); enums (`Anchor`, `IconShape`, `Sort`) as ints via `BindFunc`; arrays `ind[]`, `notif[]` registered as struct arrays (`RegisterStruct` + `RegisterArray`) and `data-for`'d; `users`/`channels` maps flattened into vectors of `{key, ...}` rebuilt on `DirtyVariable` after add/remove. Live snapshot facts (server, channel, connection, user list, profile list, update notice, key name) are a second small struct in the same model refreshed each frame by `menu::sync`.
`menu.rml`: one `<tabset>` with the 9 tabs; widgets are RmlUi form controls — `<input type="checkbox">`, `<input type="range">` (with the number shown via `{{ }}`), `<input type="text">`, `<select>` for enums, `<button>`, plus `<colorpicker>`. Collapsing headers -> a checkbox-driven `data-if`. Buttons (Save / Reload / Reset / profile Load/Overwrite/Delete/Save-as / Remove override / Customise channel) -> `BindEventCallback` calling the existing `save()` / `reload()` / `config::*_profile` code. `Users` tab selection -> `selected` string in the model. Window close `[x]` -> `data-event-click="close"` -> posts `WM_YAP_TOGGLE` like today. Drag: the document's title bar is a `<handle move_target="#document">`.
`theme.rcss`: dark pastel palette from `theme.h` as RCSS rules (window bg, tab, button, input states, rounding 5-8 px, padding 10/6). Delete `theme.h`.

### `src/colorpicker.cpp` (new) — `<colorpicker>` element
`Rml::Element` subclass. Attribute `value` = `#RRGGBBAA`. Geometry in `OnRender()` via `Rml::Mesh` with vertex colours: SV square (quad white/hue/black/black), hue bar (6 quads), alpha bar (colour->transparent over a checker of small quads), current-value swatch. `ProcessEvent` on mousedown/mousemove(drag)/mouseup maps the pointer to H/S/V/A, updates `value`, dispatches `change` with parameter `value` so `data-value` binds it like any form control. Optional colours (`text_color = 0` = unset) keep the leading checkbox from today's `color_edit(..., optional=true)` in RML (`data-if` around the picker). Size from RCSS width/height. Registered via `Factory::RegisterElementInstancer("colorpicker", ...)`.

### `src/icons.*` — SVG
`icons.cpp` becomes the 17 SVG strings (ported shapes: mic, mic-muted with slash, speaker/waves, moon, crown, star, bars, whisper, ...) served as `icons/<name>.svg` through the resource interface (`.rc` lines), all drawn in white so RCSS `image-color` tints them. `icons::svg_for(IconShape)` returns the path for the data model. `icons::draw`/`panel` are deleted (panels are RCSS `background-color` + `border-radius`). Verify in step 2 that RmlUi's `<svg>` honours `image-color` at the pinned tag; if not, substitute the fill into the SVG text in the file interface keyed by `icons/mic.svg?RRGGBB`.

## Steps

1. **Plumbing + feasibility gate**: CMake deps, `rmlui_backend`, `ui_files.cpp`, RmlUi init in `overlay.cpp`, a 3-line `hud.rml` ("hello" + one `<svg>`) rendered over the transparent swapchain with ImGui still compiled but not called. Build warning-free, run in FiveM, confirm alpha, foreground gating, and DX11 backend API match the pinned tag. Stop and re-plan if the DX11 backend or premultiplied output does not fit.
2. **HUD**: `hud.rml`, `theme.rcss`, data model in `hud.cpp`, anchoring, SVG icons, notice. Verify with demo mode (roster of every state + scripted toasts/chat).
3. **Menu**: `menu.rml`, `menu.cpp` bindings, `colorpicker.cpp`, tabs one by one (Status, Layout, Roster, Indicators, Notifications, Chat, Users, Channel, Profiles), Save/Reload/Reset/profiles wired, click-through/camera lock unchanged.
4. **Remove ImGui**: delete `theme.h`, ImGui includes, `imgui` CMake target, `icons::draw/panel`. Update `CLAUDE.md` (Architecture: UI thread, HUD, Menu paragraphs; Build deps) and write `docs/superpowers/specs/2026-09-21-rmlui-conversion-design.md` from this plan. Bump nothing yet (release is a separate step).

## Verification
- `cmake -S . -B build -A x64 && cmake --build build --config Release`: zero warnings on `YapNotifier` sources.
- `ctest --test-dir build -C Release`: `test_parser` unchanged; new `test_ui` (`tests/test_ui.cpp`) initialises Rml with a stub `RenderInterface` (only `CompileGeometry/RenderGeometry/ReleaseGeometry`), a plain-file interface pointed at `assets/ui/` on disk, loads `hud.rml` and `menu.rml`, and asserts no parse errors plus that the `hud`/`cfg` data models bind (the one runnable check that fails if RML/bindings break).
- In-game (`YAPNOTIFIER_DEPLOY_DIR` set): demo mode shows title/roster/toasts/chat at every anchor; INSERT opens the menu, cursor + camera lock work, every tab edits live, Save round-trips the INI unchanged in keys; tab out hides the overlay; `.asi` size noted in the log line (expect a few MB more from FreeType + RmlUi + LunaSVG).

## Skipped (add when needed)
- Disk override for `assets/ui/` (live-edit RCSS without rebuild): add when tuning styles gets tedious.
- Real "..." ellipsis on long names (clip for now).
- RmlUi debugger plugin.

## Implementation notes (what differed from the plan)

- Pinned: RmlUi 6.3, FreeType VER-2-14-3, LunaSVG v3.5.0 (all `GIT_SHALLOW`). FreeType's tree does not export `Freetype::Freetype` for `add_subdirectory` consumers, so `CMakeLists.txt` adds the alias itself after the fetch.
- `RenderInterface_DX11` renders into its own (MSAA) layer and composites onto our RTV in `EndFrame` with premultiplied `ONE/INV_SRC_ALPHA` on colour *and* alpha, so clearing the RTV to `{0,0,0,0}` ourselves is all the transparency needs. Its `Clear()` clears to opaque and is not used.
- `data-for` needs inner RML, so the roster icons are `<span data-for>` wrappers around an `<svg data-attr-src>` rather than `data-for` on the `<svg>` itself.
- Data views evaluate on document load, before the first `sync`, so every `View` string has a valid default (`"0dp"`, `"#FFFFFFFF"`, `"transparent"`).
- `Config::ind` / `Config::notif` became `std::array` so RmlUi can bind them as arrays; nothing else about `Config` or the INI changed.
- `test_ui` (`tests/test_ui.cpp`) is the runnable check: real `hud::init` / `menu::init`, null renderer, strict system interface that fails the test on any RmlUi warning, 30 demo frames across every anchor. Run from `assets/ui/` (ctest does).
- `font_size` now applies live (it is just `font-size` on `<body>`); only `font_file` still needs a relaunch.
