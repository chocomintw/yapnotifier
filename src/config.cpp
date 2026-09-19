#include "config.h"

#include <Windows.h>

#include <charconv>
#include <string_view>

// INI via the Win32 profile API: zero dependencies, atomic enough for a
// single-user settings file.
namespace {
constexpr wchar_t kSection[] = L"YapNotifier";

std::string read_str(const std::wstring& ini, const wchar_t* key, std::string_view def) {
    char buf[512];
    std::wstring wdef(def.begin(), def.end());
    wchar_t wbuf[512];
    DWORD n = GetPrivateProfileStringW(kSection, key, wdef.c_str(), wbuf, 512, ini.c_str());
    int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n), buf, sizeof buf, nullptr, nullptr);
    return std::string(buf, len > 0 ? len : 0);
}

template <class T>
T read_num(const std::wstring& ini, const wchar_t* key, T def) {
    std::string s = read_str(ini, key, "");
    T v = def;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

void write_str(const std::wstring& ini, const wchar_t* key, std::string_view val) {
    wchar_t wbuf[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, val.data(), static_cast<int>(val.size()), wbuf, 511);
    wbuf[n > 0 ? n : 0] = 0;
    WritePrivateProfileStringW(kSection, key, wbuf, ini.c_str());
}

template <class T>
void write_num(const std::wstring& ini, const wchar_t* key, T val) {
    write_str(ini, key, std::to_string(val));
}
}  // namespace

namespace yap::config {

Config load(const std::wstring& ini) {
    Config c;
    c.port = read_num(ini, L"port", c.port);
    c.auto_update = read_num(ini, L"auto_update", c.auto_update ? 1 : 0) != 0;
    c.safe_mode = read_num(ini, L"safe_mode", c.safe_mode ? 1 : 0) != 0;
    c.menu_key = read_num(ini, L"menu_key", c.menu_key);
    c.pos_x = read_num(ini, L"pos_x", c.pos_x);
    c.pos_y = read_num(ini, L"pos_y", c.pos_y);
    c.opacity = read_num(ini, L"opacity", c.opacity);
    c.scale = read_num(ini, L"scale", c.scale);
    return c;
}

void save(const Config& c, const std::wstring& ini) {
    write_num(ini, L"port", c.port);
    write_num(ini, L"auto_update", c.auto_update ? 1 : 0);
    write_num(ini, L"safe_mode", c.safe_mode ? 1 : 0);
    write_num(ini, L"menu_key", c.menu_key);
    write_num(ini, L"pos_x", c.pos_x);
    write_num(ini, L"pos_y", c.pos_y);
    write_num(ini, L"opacity", c.opacity);
    write_num(ini, L"scale", c.scale);
}

}  // namespace yap::config
