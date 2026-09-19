#pragma once
#include <string>

namespace yap {

struct Config {
    std::string api_key;
    std::string host = "127.0.0.1";
    int port = 25639;
    int menu_key = 0x2D;  // VK_INSERT
    float pos_x = 20.f;
    float pos_y = 20.f;
    float opacity = 0.85f;
    float scale = 1.f;
};

namespace config {
// Missing file or keys -> defaults. Never throws.
Config load(const std::wstring& ini_path);
void save(const Config& cfg, const std::wstring& ini_path);
}  // namespace config

}  // namespace yap
