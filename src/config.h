#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace yap {

// Packed like IM_COL32: A<<24 | B<<16 | G<<8 | R. 0 (fully transparent black)
// doubles as "unset" wherever a colour is optional.
using Color = uint32_t;
constexpr Color rgba(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

enum class Anchor { TopLeft, TopCenter, TopRight, MiddleLeft, MiddleCenter, MiddleRight, BottomLeft, BottomCenter, BottomRight };
enum class IconShape {
    None, Dot, Circle, Ring, Square, Diamond, Triangle, Star, Chevron, Microphone, MicrophoneMuted,
    Speaker, SpeakerMuted, Moon, Record, Crown, Whisper, Bars, Count
};
enum class Sort { ChannelOrder, Alphabetical, SpeakingFirst };

// One per user state. `text_color` 0 = leave the name colour alone.
struct Indicator {
    bool enabled = true;
    IconShape icon = IconShape::None;
    Color icon_color = rgba(255, 255, 255);
    Color text_color = 0;
    float dim = 0.f;  // 0..1, fades the whole row
};
enum Ind {
    IndSpeaking, IndWhispering, IndMicMuted, IndSpeakerMuted, IndMicHardwareOff, IndAway, IndRecording,
    IndCommander, IndPrioritySpeaker, IndSuppressed, IndLocallyMuted, IndCount
};
extern const char* const kIndNames[IndCount];  // ini key stems, also menu labels

struct NotifCategory {
    bool enabled = true;
    std::string format;
    Color color = rgba(255, 255, 255);
    int hold_ms = 4000;
};
enum Notif { NotifJoin, NotifLeave, NotifSwitch, NotifConnection, NotifWhisper, NotifChat, NotifPrivateChat, NotifPoke, NotifCount };
extern const char* const kNotifNames[NotifCount];
extern const char* const kNotifPrefixes[NotifCount];

struct UserOverride {
    Color name_color = 0;
    Color speaking_color = 0;
    IconShape icon = IconShape::None;
    Color icon_color = rgba(255, 255, 255);
    std::string display_name;
    int is_friend = -1;  // -1 = as TeamSpeak says
    std::string friend_tag;
};

struct ChannelOverride {
    Color title_color = 0;
    std::string display_name;
    IconShape icon = IconShape::None;
    Color icon_color = rgba(88, 166, 255);
    float opacity = -1.f;  // <0 = default
};

struct Config {
    // core
    int port = 25640;  // UDP port the TS3 plugin sends to (yap::proto::kPort)
    bool auto_update = true;
    int menu_key = 0x2D;  // VK_INSERT
    bool dark_theme = false;  // menu + toasts palette; light follows DESIGN.md, dark is its ink inversion
    bool demo = false;    // not saved

    // general look
    bool show_when_disconnected = true;
    float master_opacity = 0.92f;
    float scale = 1.f;
    std::string font_file;  // relative to the plugins dir; empty = embedded Inter
    float font_size = 19.f;
    bool text_shadow = true;
    bool text_outline = false;
    Color text_color = rgba(230, 233, 238);
    Color text_secondary = rgba(150, 156, 166);

    // roster block (title + list)
    Anchor anchor = Anchor::TopLeft;
    float pos_x = 20.f, pos_y = 20.f;  // offset from the anchored corner
    bool show_title = true, show_parent = true, show_count = true, show_server = false;
    std::string title_format = "{channel}";
    std::string disconnected_text = "TeamSpeak: not connected";
    bool show_panel = false;
    Color panel_color = rgba(16, 18, 22, 150);
    float corner_radius = 6.f;
    float row_spacing = 2.f;
    float icon_size = 10.f;
    Color title_color = rgba(255, 255, 255);

    bool only_show_talking = false;  // stealth: the pre-0.2 behaviour
    bool show_muted_users = true;
    bool show_local_user = true;
    bool highlight_local_user = true;
    Color local_user_color = rgba(255, 214, 102);
    Sort sort = Sort::ChannelOrder;
    int max_visible_users = 24;
    bool show_overflow_count = true;
    float max_name_width = 180.f;
    bool color_friends = true;
    Color friend_color = rgba(126, 231, 135);
    bool show_friend_tag = true;
    Color friend_tag_color = rgba(136, 200, 255);

    std::array<Indicator, IndCount> ind;  // std::array so the menu can bind it as a data array

    // animation
    int speaking_attack_ms = 90, speaking_release_ms = 260;
    bool pulse = true;
    float pulse_hz = 2.2f;
    bool fade_when_idle = false;
    int idle_after_ms = 15000;
    float idle_opacity = 0.35f;

    // notifications
    bool notif_enabled = true;
    Anchor notif_anchor = Anchor::TopRight;
    float notif_x = 24.f, notif_y = 24.f, notif_width = 280.f;
    int notif_max_visible = 5;
    bool notif_stack_up = false;
    int notif_in_ms = 200, notif_out_ms = 350;
    bool notif_merge_duplicates = true;
    int notif_suppress_after_connect_ms = 2500;
    Color notif_background = 0;  // 0 = the theme's card colour
    bool notif_accent_bar = false;
    std::array<NotifCategory, NotifCount> notif;

    // chat feed
    bool chat_enabled = false;
    Anchor chat_anchor = Anchor::BottomLeft;
    float chat_x = 24.f, chat_y = 24.f, chat_width = 420.f;
    bool chat_channel = true, chat_server = false, chat_private = false, chat_poke = true;
    int chat_max_visible = 6, chat_history = 50, chat_retention_s = 300;
    bool chat_timestamp = true, chat_sender = true, chat_channel_name = false, chat_newest_top = false;
    Color chat_sender_color = rgba(88, 166, 255);
    Color chat_background = rgba(10, 12, 16, 170);

    std::map<std::string, UserOverride> users;      // by CLIENT_UNIQUE_IDENTIFIER
    std::map<uint64_t, ChannelOverride> channels;  // by channel id

    Config();  // fills ind[] / notif[] defaults
};

namespace config {
// Missing file or keys -> defaults. Never throws.
Config load(const std::wstring& ini_path);
void save(const Config& cfg, const std::wstring& ini_path);

// Profiles are plain copies of the ini in <ini dir>/YapNotifier.profiles/<name>.ini.
std::vector<std::string> list_profiles(const std::wstring& ini_path);
bool save_profile(const std::wstring& ini_path, const std::string& name);
bool load_profile(const std::wstring& ini_path, const std::string& name);  // caller re-loads the ini
bool delete_profile(const std::wstring& ini_path, const std::string& name);

// "#RRGGBB" / "#RRGGBBAA" <-> Color. parse returns 0 on garbage.
Color parse_color(std::string_view s);
std::string format_color(Color c);
const char* to_string(Anchor a);
const char* to_string(IconShape s);
const char* to_string(Sort s);
Anchor parse_anchor(std::string_view s);
IconShape parse_icon(std::string_view s);
Sort parse_sort(std::string_view s);
}  // namespace config

}  // namespace yap
