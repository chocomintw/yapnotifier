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

void eject() {
    yap::log::info("ejecting");
    yap::overlay::stop();
    yap::ts::stop();
    yap::log::info("bye");
    yap::log::close();
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

    // Poll the menu hotkey and the END eject key. GetAsyncKeyState reads global
    // state, so this works whether or not our window has focus.
    bool menu_was_down = false, end_was_down = false;
    for (;;) {
        bool menu_down = (GetAsyncKeyState(cfg.menu_key) & 0x8000) != 0;
        if (menu_down && !menu_was_down) yap::overlay::toggle_menu();
        menu_was_down = menu_down;

        bool end_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (end_down && !end_was_down) break;
        end_was_down = end_down;

        Sleep(30);
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
