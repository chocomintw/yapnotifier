#include <Windows.h>

#include <filesystem>

#include "config.h"
#include "log.h"
#include "overlay.h"
#include "teamspeak.h"
#include "update.h"
#include "version.h"

namespace {
HMODULE g_module = nullptr;

std::filesystem::path module_path() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_module, buf, MAX_PATH);
    return buf;
}

DWORD WINAPI init_thread(LPVOID) {
    const auto self = module_path();
    const auto dir = self.parent_path();
    yap::log::open((dir / L"YapNotifier.log").wstring());
    yap::log::info("loading v" YAP_VERSION " (pid {})", GetCurrentProcessId());
    yap::update::cleanup_previous(self);

    const auto ini = (dir / L"YapNotifier.ini").wstring();
    const yap::Config cfg = yap::config::load(ini);
    yap::overlay::set_config(cfg, ini);
    yap::ts::configure(cfg.port);

    yap::overlay::start();  // own window + device; no game hooks
    yap::ts::start();
    yap::update::check_and_install(self, cfg.auto_update);

    // Poll the menu and hide-HUD hotkeys. GetAsyncKeyState reads global state, so only act on it while
    // this process (the game or our own overlay window) is in the foreground. The keys come
    // live from the overlay (rebindable in the menu). There is deliberately no eject: the
    // plugin lives for the whole process.
    auto down = [](int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool menu_was_down = false, hide_was_down = false;
    for (;;) {
        const auto keys = yap::overlay::hotkeys();
        bool menu_down = down(keys.menu);
        bool hide_down = down(keys.hide);
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
        const bool ours = fg_pid == GetCurrentProcessId();
        if (menu_down && !menu_was_down && ours) yap::overlay::toggle_menu();
        if (hide_down && !hide_was_down && ours) yap::overlay::toggle_hud();
        menu_was_down = menu_down;
        hide_was_down = hide_down;

        Sleep(30);
    }
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
