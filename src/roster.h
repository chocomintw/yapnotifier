#pragma once
// Roster filtering, ordering and per-user styling. Pure logic (no UI library) so the
// host-side test can pin the state priority rules.
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "teamspeak.h"

namespace yap::roster {

// Speaking attack/release per user, keyed by unique id. Talk state snaps on the
// wire; this turns it into a 0..1 level the colour and pulse follow.
class Envelope {
public:
    void tick(const std::vector<ts::User>& users, float dt_ms, const Config& cfg);
    float level(const std::string& uid) const;
    bool any_active() const;

private:
    std::map<std::string, float> levels_;
};

struct Icon {
    IconShape shape;
    Color color;
};

struct Resolved {
    std::string name;  // override display name or nickname
    std::string tag;   // friend tag text, empty when none/hidden
    Color name_color = 0;
    Color tag_color = 0;
    float opacity = 1.f;  // 1 - strongest active dim
    bool speaking = false;
    float level = 0.f;
    std::vector<Icon> leading, trailing;
};

// Applies show_local_user / only_show_talking / show_muted_users and `sort`.
// Own client is always first.
std::vector<const ts::User*> visible(const ts::Snapshot& snap, const Config& cfg);

Resolved resolve(const ts::User& u, const Config& cfg, float level);

Color lerp(Color a, Color b, float t);

// Keeps a block of `size` inside a viewport of `extent` (any one unit): edge anchors push
// in from that edge, so the offset is limited to [0, extent - size]; centred anchors move
// off the middle, so it is limited to +-(extent - size) / 2.
float clamp_offset(float offset, float size, float extent, bool centred);

}  // namespace yap::roster
