#include <Windows.h>

#include <filesystem>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "overlay.h"
#include "teamspeak.h"

namespace {
HMODULE g_module = nullptr;

std::filesystem::path module_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_module, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

void eject() {
    yap::log::info("ejecting");
    yap::hooks::remove();      // no new Present calls reach us after this
    yap::overlay::shutdown();  // safe now: render thread is out of our code
    yap::ts::stop();
    yap::log::info("bye");
    yap::log::close();
}

DWORD WINAPI init_thread(LPVOID) {
    const auto dir = module_dir();
    yap::log::open((dir / L"YapNotifier.log").wstring());
    yap::log::info("loading (pid {})", GetCurrentProcessId());

    const auto ini = (dir / L"YapNotifier.ini").wstring();
    const yap::Config cfg = yap::config::load(ini);
    yap::overlay::set_config(cfg, ini);
    yap::ts::configure(cfg.host, cfg.port, cfg.api_key);

    if (!yap::hooks::install()) {
        // Stay loaded but dormant; never take the game down with us.
        yap::log::error("hook install failed; plugin dormant");
        return 0;
    }
    yap::ts::start();
    yap::log::info("ready");

    // Dev convenience: END unloads the plugin so a rebuilt .asi can be dropped
    // in without restarting the game.
    bool was_down = false;
    for (;;) {
        bool down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (down && !was_down) break;
        was_down = down;
        Sleep(50);
    }
    eject();
    FreeLibraryAndExitThread(g_module, 0);
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        // Never do real work under the loader lock.
        if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
