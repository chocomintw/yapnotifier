#pragma once
// SPDX-License-Identifier: MIT
// Vector icon painter, ported from TeamSpeak3-Reshade-overlay (assets/LICENSE-tsro.txt).
// Every indicator is ImDrawList primitives: no textures, scales to any size.
#include "config.h"

struct ImDrawList;

namespace yap::icons {

// Draws `shape` centred on (cx, cy) inside a `size` px box. IconShape::None draws nothing.
void draw(ImDrawList* dl, IconShape shape, float cx, float cy, float size, Color color, float thickness = 1.5f);

// Rounded rectangle with optional border (alpha 0 = skipped).
void panel(ImDrawList* dl, float x, float y, float w, float h, float rounding, Color fill, Color border,
           float border_thickness);

Color with_alpha(Color c, float scale);

}  // namespace yap::icons
