#include "log.h"

#include <Windows.h>

#include <fstream>
#include <mutex>

namespace {
std::ofstream g_file;
std::mutex g_mutex;
}  // namespace

namespace yap::log {

void open(const std::wstring& path) {
    std::lock_guard lk(g_mutex);
    if (!g_file.is_open()) g_file.open(path, std::ios::app);
}

void close() {
    std::lock_guard lk(g_mutex);
    g_file.close();
}

void write(std::string_view level, std::string_view msg) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lk(g_mutex);
    if (!g_file.is_open()) return;
    g_file << std::format("{:02}:{:02}:{:02}.{:03} [{}] {}\n",
                          t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, msg)
           << std::flush;
}

}  // namespace yap::log
