#include "roster.h"

#include <algorithm>
#include <cctype>

#include "yap_protocol.h"

namespace yap::roster {
namespace proto = yap::proto;

float clamp_offset(float offset, float size, float extent, bool centred) {
    const float room = std::max(extent - size, 0.f);
    return centred ? std::clamp(offset, -room / 2.f, room / 2.f) : std::clamp(offset, 0.f, room);
}

Color lerp(Color a, Color b, float t) {
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    Color out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF), cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<Color>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

void Envelope::tick(const std::vector<ts::User>& users, float dt_ms, const Config& cfg) {
    std::map<std::string, float> next;
    for (const auto& u : users) {
        float lv = 0.f;
        if (auto it = levels_.find(u.uid); it != levels_.end()) lv = it->second;
        const bool talking = u.flags & proto::Talking;
        const float ms = static_cast<float>(talking ? cfg.speaking_attack_ms : cfg.speaking_release_ms);
        const float step = ms <= 0.f ? 1.f : dt_ms / ms;
        lv = talking ? std::min(1.f, lv + step) : std::max(0.f, lv - step);
        next[u.uid] = lv;
    }
    levels_ = std::move(next);
}

float Envelope::level(const std::string& uid) const {
    auto it = levels_.find(uid);
    return it == levels_.end() ? 0.f : it->second;
}

bool Envelope::any_active() const {
    for (const auto& [uid, lv] : levels_)
        if (lv > 0.f) return true;
    return false;
}

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

std::vector<const ts::User*> visible(const ts::Snapshot& snap, const Config& cfg) {
    std::vector<const ts::User*> out;
    for (const auto& u : snap.users) {
        const bool self = u.flags & proto::Self;
        if (self && !cfg.show_local_user) continue;
        if (!self && cfg.only_show_talking && !(u.flags & proto::Talking)) continue;
        if (!self && !cfg.show_muted_users && (u.flags & (proto::InputMuted | proto::OutputMuted))) continue;
        out.push_back(&u);
    }
    auto by_name = [](const ts::User* a, const ts::User* b) { return lower(a->nickname) < lower(b->nickname); };
    if (cfg.sort == Sort::Alphabetical) std::stable_sort(out.begin(), out.end(), by_name);
    if (cfg.sort == Sort::SpeakingFirst)
        std::stable_partition(out.begin(), out.end(), [](const ts::User* u) { return u->flags & proto::Talking; });
    std::stable_partition(out.begin(), out.end(), [](const ts::User* u) { return u->flags & proto::Self; });
    return out;
}

Resolved resolve(const ts::User& u, const Config& cfg, float level) {
    Resolved r;
    const UserOverride* ov = nullptr;
    if (auto it = cfg.users.find(u.uid); it != cfg.users.end()) ov = &it->second;

    r.name = ov && !ov->display_name.empty() ? ov->display_name : u.nickname;
    r.level = level;
    r.speaking = u.flags & proto::Talking;

    const bool self = u.flags & proto::Self;
    bool is_friend = u.flags & proto::Friend;
    if (ov && ov->is_friend >= 0) is_friend = ov->is_friend != 0;
    if (is_friend && cfg.show_friend_tag) {
        r.tag = ov && !ov->friend_tag.empty() ? ov->friend_tag : u.contact_nick;
        r.tag_color = cfg.friend_tag_color;
    }

    Color color = cfg.text_color;
    if (is_friend && cfg.color_friends) color = cfg.friend_color;
    if (self && cfg.highlight_local_user) color = cfg.local_user_color;
    if (ov && ov->name_color) color = ov->name_color;

    // State text colours, later wins (mirrors the reference priority order).
    float dim = 0.f;
    auto apply = [&](Ind i, bool active) {
        const Indicator& ind = cfg.ind[i];
        if (!active || !ind.enabled) return;
        if (ind.text_color && i != IndSpeaking) color = ind.text_color;
        dim = std::max(dim, ind.dim);
    };
    apply(IndAway, u.flags & proto::Away);
    apply(IndSuppressed, u.flags & proto::Suppressed);
    apply(IndLocallyMuted, u.flags & proto::LocallyMuted);
    apply(IndMicMuted, (u.flags & proto::InputMuted) || (u.flags & proto::HardwareOff));
    apply(IndSpeakerMuted, u.flags & proto::OutputMuted);
    apply(IndMicHardwareOff, u.flags & proto::HardwareOff);
    apply(IndWhispering, u.flags & proto::Whisper);
    if (cfg.ind[IndSpeaking].enabled && level > 0.f) {
        Color target = ov && ov->speaking_color ? ov->speaking_color : cfg.ind[IndSpeaking].text_color;
        if (target) color = lerp(color, target, level);
    }
    if (cfg.ind[IndWhispering].enabled && (u.flags & proto::Whisper) && cfg.ind[IndWhispering].text_color)
        color = cfg.ind[IndWhispering].text_color;
    r.name_color = color;
    r.opacity = 1.f - dim;

    auto icon = [&](Ind i, bool active, std::vector<Icon>& dst) {
        const Indicator& ind = cfg.ind[i];
        if (active && ind.enabled && ind.icon != IconShape::None) dst.push_back({ind.icon, ind.icon_color});
    };
    icon(IndCommander, u.flags & proto::Commander, r.leading);
    icon(IndPrioritySpeaker, u.flags & proto::Priority, r.leading);
    if (ov && ov->icon != IconShape::None) r.leading.push_back({ov->icon, ov->icon_color});

    icon(IndWhispering, u.flags & proto::Whisper, r.trailing);
    icon(IndMicMuted, u.flags & proto::InputMuted, r.trailing);
    icon(IndSpeakerMuted, u.flags & proto::OutputMuted, r.trailing);
    icon(IndMicHardwareOff, u.flags & proto::HardwareOff, r.trailing);
    icon(IndAway, u.flags & proto::Away, r.trailing);
    icon(IndRecording, u.flags & proto::Recording, r.trailing);
    icon(IndLocallyMuted, u.flags & proto::LocallyMuted, r.trailing);
    icon(IndSuppressed, u.flags & proto::Suppressed, r.trailing);
    icon(IndSpeaking, level > 0.f, r.trailing);
    return r;
}

}  // namespace yap::roster
