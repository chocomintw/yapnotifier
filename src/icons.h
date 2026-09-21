#pragma once
// Indicator icons are white SVGs in assets/ui/icons (embedded as resources, see
// YapNotifier.rc) that the RCSS tints with image-color, so they scale to any size.
#include <string>

#include "config.h"

namespace yap::icons {

// Resource path for `shape` ("icons/microphone.svg"); "" for IconShape::None.
std::string svg(IconShape shape);

Color with_alpha(Color c, float scale);

}  // namespace yap::icons
