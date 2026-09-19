# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

YapNotifier: a TeamSpeak 3 "who's talking" ImGui overlay for FiveM, built as an x64 `.asi` plugin (a DLL with a renamed extension that FiveM's ASI loader picks up from its `plugins/` folder). Design spec: `docs/superpowers/specs/2026-09-19-yapnotifier-design.md`.

## Build

Requires MSVC (VS 2022 x64) and CMake ≥ 3.24. Deps (MinHook, Dear ImGui) are fetched by CMake; no submodules.

```
cmake -S . -B build -A x64 [-DYAPNOTIFIER_DEPLOY_DIR="C:/path/to/FiveM/plugins"]
cmake --build build --config Release
```

Output: `build/Release/YapNotifier.asi`. With `YAPNOTIFIER_DEPLOY_DIR` set it is copied there after every build. Compiles with `/W4 /permissive-`; keep it warning-free.

There is no test suite — hooks and overlay can only be verified in-game. Runtime log: `<plugins>/YapNotifier.log`; settings: `<plugins>/YapNotifier.ini`. Press **INSERT** in-game for the config menu, **END** to eject the plugin (dev hot-reload).

## Architecture

Three threads, one direction of data flow:

- **Init/eject thread** (`main.cpp`): spawned from `DllMain` (never do work under the loader lock). Loads config, installs hooks, starts the TS thread, then polls for END to tear everything down in reverse order and `FreeLibraryAndExitThread`.
- **Render thread** (`hooks.cpp` → `overlay.cpp`): the game's own thread, entered via MinHook detours on `IDXGISwapChain::Present` (slot 8) and `::ResizeBuffers` (slot 13). Vtable addresses come from a throwaway D3D11 swapchain on a hidden window — every swapchain in the process shares dxgi's vtable, so no pattern scanning. `overlay` lazily initialises ImGui from the real swapchain on first Present, subclasses the game window's WndProc for input, and must release the backbuffer RTV in `on_resize()` *before* the original ResizeBuffers runs.
- **TeamSpeak thread** (`teamspeak.cpp`): connects to the ClientQuery plugin (TCP 127.0.0.1:25639), owns the mutable `clid → Client` map, and publishes immutable `Snapshot`s through a single `std::atomic<std::shared_ptr<const Snapshot>>`. The render thread only ever calls `ts::snapshot()`. Reconnects forever with backoff; any failure just yields an empty snapshot.

Failure policy everywhere: log and go dormant, never crash the game.

The ClientQuery protocol handling (`handle_line` in `teamspeak.cpp`) is a stub with the expected line formats documented above it.
