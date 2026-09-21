#include "config.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>

// INI via the Win32 profile API: zero dependencies, atomic enough for a
// single-user settings file. One field list (`visit`) drives both load and save.
namespace yap {

const char* const kIndNames[IndCount] = {"speaking",  "whispering",       "mic_muted",  "speaker_muted",
                                         "mic_hardware_off", "away",     "recording",  "commander",
                                         "priority_speaker", "suppressed", "locally_muted"};
const char* const kNotifNames[NotifCount] = {"join", "leave", "switch", "connection",
                                             "whisper", "chat", "private_chat", "poke"};
const char* const kNotifPrefixes[NotifCount] = {"[+]", "[-]", "[>]", "[*]", "[w]", "[#]", "[PM]", "[POKE]"};

namespace {
constexpr wchar_t kSection[] = L"YapNotifier";
constexpr wchar_t kMissing[] = L"\x7f";  // sentinel default: key absent

constexpr Color kGreen = rgba(126, 231, 135), kOrange = rgba(255, 149, 43), kRed = rgba(240, 104, 104),
                kPurple = rgba(193, 122, 255), kAmber = rgba(224, 184, 92), kRecRed = rgba(255, 85, 85),
                kBlue = rgba(88, 166, 255), kCyan = rgba(86, 214, 214), kGrey = rgba(120, 126, 136);

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

template <class T>
bool to_num(std::string_view s, T& v) {
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{};
}

// --- load ---------------------------------------------------------------------
struct Reader {
    const std::wstring& ini;
    const wchar_t* section;

    bool raw(const std::wstring& key, std::string& out) const {
        wchar_t buf[2048];
        GetPrivateProfileStringW(section, key.c_str(), kMissing, buf, 2048, ini.c_str());
        if (std::wstring_view(buf) == kMissing) return false;
        out = narrow(buf);
        return true;
    }
    void operator()(const std::wstring& k, int& v) const {
        std::string s;
        if (raw(k, s)) to_num(s, v);
    }
    void operator()(const std::wstring& k, float& v) const {
        std::string s;
        if (raw(k, s)) to_num(s, v);
    }
    void operator()(const std::wstring& k, bool& v) const {
        int i = v ? 1 : 0;
        (*this)(k, i);
        v = i != 0;
    }
    void operator()(const std::wstring& k, std::string& v) const {
        std::string s;
        if (raw(k, s)) v = s;
    }
    void operator()(const std::wstring& k, Color& v) const {
        std::string s;
        if (!raw(k, s)) return;
        v = s.empty() ? 0 : config::parse_color(s);
    }
    void operator()(const std::wstring& k, Anchor& v) const {
        std::string s;
        if (raw(k, s)) v = config::parse_anchor(s);
    }
    void operator()(const std::wstring& k, IconShape& v) const {
        std::string s;
        if (raw(k, s)) v = config::parse_icon(s);
    }
    void operator()(const std::wstring& k, Sort& v) const {
        std::string s;
        if (raw(k, s)) v = config::parse_sort(s);
    }
};

// --- save ---------------------------------------------------------------------
struct Writer {
    const std::wstring& ini;
    const wchar_t* section;

    void raw(const std::wstring& key, std::string_view val) const {
        WritePrivateProfileStringW(section, key.c_str(), widen(val).c_str(), ini.c_str());
    }
    void operator()(const std::wstring& k, int& v) const { raw(k, std::to_string(v)); }
    void operator()(const std::wstring& k, float& v) const {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
        raw(k, buf);
    }
    void operator()(const std::wstring& k, bool& v) const { raw(k, v ? "1" : "0"); }
    void operator()(const std::wstring& k, std::string& v) const { raw(k, v); }
    void operator()(const std::wstring& k, Color& v) const { raw(k, v ? config::format_color(v) : ""); }
    void operator()(const std::wstring& k, Anchor& v) const { raw(k, config::to_string(v)); }
    void operator()(const std::wstring& k, IconShape& v) const { raw(k, config::to_string(v)); }
    void operator()(const std::wstring& k, Sort& v) const { raw(k, config::to_string(v)); }
};

// --- the field list ---------------------------------------------------------------
template <class F>
void visit(Config& c, const F& f) {
    f(L"port", c.port);
    f(L"auto_update", c.auto_update);
    f(L"menu_key", c.menu_key);
    f(L"menu_dark", c.menu_dark);

    f(L"show_when_disconnected", c.show_when_disconnected);
    f(L"master_opacity", c.master_opacity);
    f(L"scale", c.scale);
    f(L"font_file", c.font_file);
    f(L"font_size", c.font_size);
    f(L"text_shadow", c.text_shadow);
    f(L"text_outline", c.text_outline);
    f(L"text_color", c.text_color);
    f(L"text_secondary", c.text_secondary);

    f(L"anchor", c.anchor);
    f(L"pos_x", c.pos_x);
    f(L"pos_y", c.pos_y);
    f(L"show_title", c.show_title);
    f(L"show_parent", c.show_parent);
    f(L"show_count", c.show_count);
    f(L"show_server", c.show_server);
    f(L"title_format", c.title_format);
    f(L"disconnected_text", c.disconnected_text);
    f(L"show_panel", c.show_panel);
    f(L"panel_color", c.panel_color);
    f(L"corner_radius", c.corner_radius);
    f(L"row_spacing", c.row_spacing);
    f(L"icon_size", c.icon_size);
    f(L"title_color", c.title_color);

    f(L"only_show_talking", c.only_show_talking);
    f(L"show_muted_users", c.show_muted_users);
    f(L"show_local_user", c.show_local_user);
    f(L"highlight_local_user", c.highlight_local_user);
    f(L"local_user_color", c.local_user_color);
    f(L"sort", c.sort);
    f(L"max_visible_users", c.max_visible_users);
    f(L"show_overflow_count", c.show_overflow_count);
    f(L"max_name_width", c.max_name_width);
    f(L"color_friends", c.color_friends);
    f(L"friend_color", c.friend_color);
    f(L"show_friend_tag", c.show_friend_tag);
    f(L"friend_tag_color", c.friend_tag_color);

    for (int i = 0; i < IndCount; ++i) {
        std::wstring k = L"ind_" + widen(kIndNames[i]) + L"_";
        f(k + L"enabled", c.ind[i].enabled);
        f(k + L"icon", c.ind[i].icon);
        f(k + L"icon_color", c.ind[i].icon_color);
        f(k + L"text_color", c.ind[i].text_color);
        f(k + L"dim", c.ind[i].dim);
    }

    f(L"speaking_attack_ms", c.speaking_attack_ms);
    f(L"speaking_release_ms", c.speaking_release_ms);
    f(L"pulse", c.pulse);
    f(L"pulse_hz", c.pulse_hz);
    f(L"fade_when_idle", c.fade_when_idle);
    f(L"idle_after_ms", c.idle_after_ms);
    f(L"idle_opacity", c.idle_opacity);

    f(L"notif_enabled", c.notif_enabled);
    f(L"notif_anchor", c.notif_anchor);
    f(L"notif_x", c.notif_x);
    f(L"notif_y", c.notif_y);
    f(L"notif_width", c.notif_width);
    f(L"notif_max_visible", c.notif_max_visible);
    f(L"notif_stack_up", c.notif_stack_up);
    f(L"notif_in_ms", c.notif_in_ms);
    f(L"notif_out_ms", c.notif_out_ms);
    f(L"notif_merge_duplicates", c.notif_merge_duplicates);
    f(L"notif_suppress_after_connect_ms", c.notif_suppress_after_connect_ms);
    f(L"notif_background", c.notif_background);
    f(L"notif_accent_bar", c.notif_accent_bar);
    for (int i = 0; i < NotifCount; ++i) {
        std::wstring k = L"notif_" + widen(kNotifNames[i]) + L"_";
        f(k + L"enabled", c.notif[i].enabled);
        f(k + L"format", c.notif[i].format);
        f(k + L"color", c.notif[i].color);
        f(k + L"hold_ms", c.notif[i].hold_ms);
    }

    f(L"chat_enabled", c.chat_enabled);
    f(L"chat_anchor", c.chat_anchor);
    f(L"chat_x", c.chat_x);
    f(L"chat_y", c.chat_y);
    f(L"chat_width", c.chat_width);
    f(L"chat_channel", c.chat_channel);
    f(L"chat_server", c.chat_server);
    f(L"chat_private", c.chat_private);
    f(L"chat_poke", c.chat_poke);
    f(L"chat_max_visible", c.chat_max_visible);
    f(L"chat_history", c.chat_history);
    f(L"chat_retention_s", c.chat_retention_s);
    f(L"chat_timestamp", c.chat_timestamp);
    f(L"chat_sender", c.chat_sender);
    f(L"chat_channel_name", c.chat_channel_name);
    f(L"chat_newest_top", c.chat_newest_top);
    f(L"chat_sender_color", c.chat_sender_color);
    f(L"chat_background", c.chat_background);
}

template <class F>
void visit(UserOverride& u, const F& f) {
    f(L"name_color", u.name_color);
    f(L"speaking_color", u.speaking_color);
    f(L"icon", u.icon);
    f(L"icon_color", u.icon_color);
    f(L"display_name", u.display_name);
    f(L"is_friend", u.is_friend);
    f(L"friend_tag", u.friend_tag);
}

template <class F>
void visit(ChannelOverride& ch, const F& f) {
    f(L"title_color", ch.title_color);
    f(L"display_name", ch.display_name);
    f(L"icon", ch.icon);
    f(L"icon_color", ch.icon_color);
    f(L"opacity", ch.opacity);
}

std::vector<std::wstring> override_sections(const std::wstring& ini) {
    std::vector<std::wstring> out;
    std::vector<wchar_t> buf(32768);
    DWORD n = GetPrivateProfileSectionNamesW(buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        std::wstring_view s(p);
        if (s.starts_with(L"user:") || s.starts_with(L"channel:")) out.emplace_back(s);
    }
    return out;
}

template <class T>
T clampv(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }

void clamp(Config& c) {
    c.master_opacity = clampv(c.master_opacity, 0.f, 1.f);
    c.scale = clampv(c.scale, 0.25f, 4.f);
    c.font_size = clampv(c.font_size, 6.f, 96.f);
    c.corner_radius = clampv(c.corner_radius, 0.f, 32.f);
    c.row_spacing = clampv(c.row_spacing, 0.f, 64.f);
    c.icon_size = clampv(c.icon_size, 2.f, 96.f);
    c.max_visible_users = clampv(c.max_visible_users, 1, 512);
    c.max_name_width = clampv(c.max_name_width, 20.f, 2000.f);
    for (auto& i : c.ind) i.dim = clampv(i.dim, 0.f, 1.f);
    c.speaking_attack_ms = clampv(c.speaking_attack_ms, 0, 5000);
    c.speaking_release_ms = clampv(c.speaking_release_ms, 0, 5000);
    c.pulse_hz = clampv(c.pulse_hz, 0.1f, 10.f);
    c.idle_after_ms = clampv(c.idle_after_ms, 1000, 600000);
    c.idle_opacity = clampv(c.idle_opacity, 0.f, 1.f);
    c.notif_width = clampv(c.notif_width, 80.f, 2000.f);
    c.notif_max_visible = clampv(c.notif_max_visible, 1, 20);
    c.notif_in_ms = clampv(c.notif_in_ms, 0, 5000);
    c.notif_out_ms = clampv(c.notif_out_ms, 0, 5000);
    c.notif_suppress_after_connect_ms = clampv(c.notif_suppress_after_connect_ms, 0, 60000);
    for (auto& n : c.notif) n.hold_ms = clampv(n.hold_ms, 0, 60000);
    c.chat_width = clampv(c.chat_width, 80.f, 2000.f);
    c.chat_history = clampv(c.chat_history, 1, 500);
    c.chat_max_visible = clampv(c.chat_max_visible, 1, c.chat_history);
    c.chat_retention_s = clampv(c.chat_retention_s, 0, 86400);
    for (auto& [id, ch] : c.channels)
        if (ch.opacity >= 0.f) ch.opacity = clampv(ch.opacity, 0.f, 1.f);
}

std::wstring profiles_dir(const std::wstring& ini) {
    return (std::filesystem::path(ini).parent_path() / L"YapNotifier.profiles").wstring();
}

std::wstring profile_path(const std::wstring& ini, const std::string& name) {
    std::string safe;
    for (char ch : name)
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '_' || ch == '-') safe += ch;
    if (safe.empty()) return {};
    return profiles_dir(ini) + L"\\" + widen(safe) + L".ini";
}
}  // namespace

Config::Config() {
    auto set = [this](Ind i, IconShape icon, Color ic, Color tc, float dim, bool enabled = true) {
        ind[i] = {enabled, icon, ic, tc, dim};
    };
    set(IndSpeaking, IconShape::None, kGreen, kGreen, 0.f);
    set(IndWhispering, IconShape::Whisper, kCyan, kCyan, 0.f);
    set(IndMicMuted, IconShape::MicrophoneMuted, kRed, rgba(164, 168, 176), 0.55f);
    set(IndSpeakerMuted, IconShape::SpeakerMuted, kPurple, rgba(150, 140, 168), 0.5f);
    set(IndMicHardwareOff, IconShape::Microphone, kGrey, 0, 0.f, false);
    set(IndAway, IconShape::Moon, kAmber, rgba(176, 166, 140), 0.45f);
    set(IndRecording, IconShape::Record, kRecRed, kRecRed, 0.f);
    set(IndCommander, IconShape::Circle, kOrange, rgba(255, 197, 132), 0.f);
    set(IndPrioritySpeaker, IconShape::Chevron, kBlue, 0, 0.f);
    set(IndSuppressed, IconShape::Diamond, kGrey, kGrey, 0.6f, false);
    set(IndLocallyMuted, IconShape::Square, kGrey, kGrey, 0.5f);

    auto n = [this](Notif i, bool enabled, const char* fmt, Color col, int hold) {
        notif[i] = {enabled, fmt, col, hold};
    };
    n(NotifJoin, true, "{name} joined from {from}", kGreen, 4000);
    n(NotifLeave, true, "{name} left to {to}", kRed, 4000);
    n(NotifSwitch, true, "{previous} -> {channel} ({count})", kBlue, 3000);
    n(NotifConnection, true, "TeamSpeak: {status}", kAmber, 4000);
    n(NotifWhisper, true, "{name} is whispering from {channel}", kCyan, 2500);
    n(NotifChat, false, "{name}: {message}", kBlue, 5000);
    n(NotifPrivateChat, false, "{name}: {message}", rgba(197, 154, 255), 8000);
    n(NotifPoke, true, "{name}: {message}", rgba(255, 197, 132), 10000);
}

namespace config {

Config load(const std::wstring& ini) {
    Config c;
    visit(c, Reader{ini, kSection});
    for (const auto& sec : override_sections(ini)) {
        Reader r{ini, sec.c_str()};
        if (sec.starts_with(L"user:")) {
            UserOverride u;
            visit(u, r);
            c.users[narrow(sec.substr(5))] = std::move(u);
        } else {
            uint64_t id = 0;
            if (!to_num(narrow(sec.substr(8)), id)) continue;
            ChannelOverride ch;
            visit(ch, r);
            c.channels[id] = std::move(ch);
        }
    }
    clamp(c);
    return c;
}

void save(const Config& cfg, const std::wstring& ini) {
    Config& c = const_cast<Config&>(cfg);  // Writer only reads
    visit(c, Writer{ini, kSection});
    for (const auto& sec : override_sections(ini))
        WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, ini.c_str());  // drop stale sections
    for (auto& [uid, u] : c.users) {
        std::wstring sec = L"user:" + widen(uid);
        visit(u, Writer{ini, sec.c_str()});
    }
    for (auto& [id, ch] : c.channels) {
        std::wstring sec = L"channel:" + std::to_wstring(id);
        visit(ch, Writer{ini, sec.c_str()});
    }
}

std::vector<std::string> list_profiles(const std::wstring& ini) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(profiles_dir(ini), ec))
        if (e.path().extension() == L".ini") out.push_back(narrow(e.path().stem().wstring()));
    std::sort(out.begin(), out.end());
    return out;
}

bool save_profile(const std::wstring& ini, const std::string& name) {
    std::wstring dst = profile_path(ini, name);
    if (dst.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(profiles_dir(ini), ec);
    return CopyFileW(ini.c_str(), dst.c_str(), FALSE) != 0;
}

bool load_profile(const std::wstring& ini, const std::string& name) {
    std::wstring src = profile_path(ini, name);
    return !src.empty() && CopyFileW(src.c_str(), ini.c_str(), FALSE) != 0;
}

bool delete_profile(const std::wstring& ini, const std::string& name) {
    std::wstring p = profile_path(ini, name);
    return !p.empty() && DeleteFileW(p.c_str()) != 0;
}

Color parse_color(std::string_view s) {
    if (s.empty() || s[0] != '#' || (s.size() != 7 && s.size() != 9)) return 0;
    unsigned v = 0;
    auto [end, ec] = std::from_chars(s.data() + 1, s.data() + s.size(), v, 16);
    if (ec != std::errc{} || end != s.data() + s.size()) return 0;
    if (s.size() == 7) v = (v << 8) | 0xFF;
    return rgba((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

std::string format_color(Color c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF);
    return buf;
}

namespace {
constexpr const char* kAnchors[] = {"top_left",    "top_center",    "top_right",   "middle_left",  "middle_center",
                                    "middle_right", "bottom_left",  "bottom_center", "bottom_right"};
constexpr const char* kIcons[] = {"none",     "dot",     "circle",  "ring",   "square",  "diamond", "triangle",
                                  "star",     "chevron", "microphone", "microphone_muted", "speaker", "speaker_muted",
                                  "moon",     "record",  "crown",   "whisper", "bars"};
constexpr const char* kSorts[] = {"channel_order", "alphabetical", "speaking_first"};

template <size_t N>
int index_of(const char* const (&names)[N], std::string_view s) {
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return static_cast<int>(i);
    return -1;
}
}  // namespace

const char* to_string(Anchor a) { return kAnchors[static_cast<int>(a)]; }
const char* to_string(IconShape s) { return kIcons[static_cast<int>(s)]; }
const char* to_string(Sort s) { return kSorts[static_cast<int>(s)]; }
Anchor parse_anchor(std::string_view s) {
    int i = index_of(kAnchors, s);
    return i < 0 ? Anchor::TopLeft : static_cast<Anchor>(i);
}
IconShape parse_icon(std::string_view s) {
    int i = index_of(kIcons, s);
    return i < 0 ? IconShape::None : static_cast<IconShape>(i);
}
Sort parse_sort(std::string_view s) {
    int i = index_of(kSorts, s);
    return i < 0 ? Sort::ChannelOrder : static_cast<Sort>(i);
}

}  // namespace config
}  // namespace yap
