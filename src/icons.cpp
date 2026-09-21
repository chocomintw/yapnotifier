#include "icons.h"

namespace yap::icons {

std::string svg(IconShape shape) {
    if (shape == IconShape::None || shape >= IconShape::Count) return {};
    return std::string("icons/") + config::to_string(shape) + ".svg";  // same names as the ini
}

Color with_alpha(Color c, float scale) {
    float a = static_cast<float>((c >> 24) & 0xFF) * scale;
    a = a < 0.f ? 0.f : a > 255.f ? 255.f : a;
    return (c & 0x00FFFFFFu) | (static_cast<Color>(a) << 24);
}

}  // namespace yap::icons
