#pragma once
#include <format>
#include <string>
#include <string_view>

// File logger. No console in a game process, so everything goes to
// <plugins>/YapNotifier.log. Safe to call from any thread; silently no-ops
// before open() / after close().
namespace yap::log {

void open(const std::wstring& path);
void close();
void write(std::string_view level, std::string_view msg);

template <class... A>
void info(std::format_string<A...> fmt, A&&... a) {
    write("INFO", std::format(fmt, std::forward<A>(a)...));
}
template <class... A>
void error(std::format_string<A...> fmt, A&&... a) {
    write("ERROR", std::format(fmt, std::forward<A>(a)...));
}

}  // namespace yap::log
