// SPDX-License-Identifier: MIT
// Ported from TeamSpeak3-Reshade-overlay reshade-integration/src/icons.cpp.
#include "icons.h"

#include <imgui.h>

#include <cmath>

namespace yap::icons {
namespace {
constexpr float kPi = 3.14159265358979323846f;

void microphone(ImDrawList* dl, float cx, float cy, float size, Color color, float thickness) {
    const float half = size * 0.5f;
    const float capsule_w = size * 0.28f;
    const float capsule_h = size * 0.46f;
    const float top = cy - half * 0.86f;
    dl->AddRectFilled({cx - capsule_w, top}, {cx + capsule_w, top + capsule_h}, color, capsule_w);
    dl->PathArcTo({cx, top + capsule_h * 0.55f}, half * 0.62f, 0.15f * kPi, 0.85f * kPi, 12);
    dl->PathStroke(color, ImDrawFlags_None, thickness);
    dl->AddLine({cx, top + capsule_h * 0.55f + half * 0.62f}, {cx, cy + half * 0.86f}, color, thickness);
}

void speaker(ImDrawList* dl, float cx, float cy, float size, Color color, float thickness, bool waves) {
    const float half = size * 0.5f;
    const float body = half * 0.42f;
    dl->AddRectFilled({cx - half * 0.8f, cy - body * 0.6f}, {cx - half * 0.25f, cy + body * 0.6f}, color, 1.f);
    dl->AddTriangleFilled({cx - half * 0.25f, cy - half * 0.7f}, {cx - half * 0.25f, cy + half * 0.7f},
                          {cx + half * 0.1f, cy}, color);
    dl->AddTriangleFilled({cx - half * 0.25f, cy - half * 0.7f}, {cx + half * 0.1f, cy},
                          {cx + half * 0.1f, cy - half * 0.7f}, color);
    dl->AddTriangleFilled({cx - half * 0.25f, cy + half * 0.7f}, {cx + half * 0.1f, cy},
                          {cx + half * 0.1f, cy + half * 0.7f}, color);
    if (!waves) return;
    for (int i = 1; i <= 2; ++i) {
        const float radius = half * (0.28f + 0.26f * static_cast<float>(i));
        dl->PathArcTo({cx + half * 0.05f, cy}, radius, -0.32f * kPi, 0.32f * kPi, 10);
        dl->PathStroke(with_alpha(color, 1.f - 0.25f * static_cast<float>(i - 1)), ImDrawFlags_None, thickness);
    }
}

// Diagonal bar negating an icon, with a dark backing stroke so it reads on any colour.
void slash(ImDrawList* dl, float cx, float cy, float size, Color color, float thickness) {
    const float half = size * 0.55f;
    dl->AddLine({cx - half, cy - half}, {cx + half, cy + half}, 0xC0000000, thickness + 1.6f);
    dl->AddLine({cx - half, cy - half}, {cx + half, cy + half}, color, thickness);
}

void star(ImDrawList* dl, float cx, float cy, float size, Color color) {
    const float outer = size * 0.5f, inner = outer * 0.45f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float r = (i % 2 == 0) ? outer : inner;
        const float a = -kPi * 0.5f + static_cast<float>(i) * kPi / 5.f;
        pts[i] = {cx + std::cos(a) * r, cy + std::sin(a) * r};
    }
    dl->AddConvexPolyFilled(pts, 10, color);
}

void crown(ImDrawList* dl, float cx, float cy, float size, Color color) {
    const float half = size * 0.5f;
    const ImVec2 bl{cx - half, cy + half * 0.55f}, br{cx + half, cy + half * 0.55f};
    dl->AddTriangleFilled(bl, br, {cx, cy - half * 0.1f}, color);
    dl->AddTriangleFilled(bl, {cx - half * 0.33f, cy}, {cx - half, cy - half * 0.7f}, color);
    dl->AddTriangleFilled(br, {cx + half * 0.33f, cy}, {cx + half, cy - half * 0.7f}, color);
    dl->AddTriangleFilled({cx - half * 0.33f, cy}, {cx + half * 0.33f, cy}, {cx, cy - half * 0.95f}, color);
}

void bars(ImDrawList* dl, float cx, float cy, float size, Color color) {
    const float half = size * 0.5f, width = size * 0.18f;
    const float heights[3] = {0.55f, 1.f, 0.72f};
    for (int i = 0; i < 3; ++i) {
        const float x = cx + (static_cast<float>(i) - 1.f) * size * 0.3f;
        const float h = half * heights[i];
        dl->AddRectFilled({x - width * 0.5f, cy - h}, {x + width * 0.5f, cy + h}, color, width * 0.4f);
    }
}

void moon(ImDrawList* dl, float cx, float cy, float size, Color color) {
    const float r = size * 0.5f;
    dl->PathArcTo({cx, cy}, r, kPi * 0.42f, kPi * 1.58f, 16);
    dl->PathArcTo({cx + r * 0.52f, cy}, r * 0.92f, kPi * 1.35f, kPi * 0.65f, 16);
    dl->PathFillConvex(color);
}

void whisper(ImDrawList* dl, float cx, float cy, float size, Color color) {
    const float half = size * 0.5f;
    dl->AddRectFilled({cx - half, cy - half * 0.78f}, {cx + half, cy + half * 0.22f}, color, half * 0.35f);
    dl->AddTriangleFilled({cx - half * 0.32f, cy + half * 0.18f}, {cx + half * 0.12f, cy + half * 0.18f},
                          {cx - half * 0.52f, cy + half * 0.85f}, color);
}
}  // namespace

Color with_alpha(Color c, float scale) {
    float a = static_cast<float>((c >> 24) & 0xFF) * scale;
    a = a < 0.f ? 0.f : a > 255.f ? 255.f : a;
    return (c & 0x00FFFFFFu) | (static_cast<Color>(a) << 24);
}

void draw(ImDrawList* dl, IconShape shape, float cx, float cy, float size, Color color, float thickness) {
    if (!dl || shape == IconShape::None || size <= 0.f) return;
    const float half = size * 0.5f;
    switch (shape) {
        case IconShape::Dot: dl->AddCircleFilled({cx, cy}, half * 0.45f, color, 12); return;
        case IconShape::Circle: dl->AddCircleFilled({cx, cy}, half, color, 16); return;
        case IconShape::Ring: dl->AddCircle({cx, cy}, half * 0.86f, color, 16, thickness); return;
        case IconShape::Square:
            dl->AddRectFilled({cx - half * 0.8f, cy - half * 0.8f}, {cx + half * 0.8f, cy + half * 0.8f}, color,
                              half * 0.22f);
            return;
        case IconShape::Diamond: {
            const ImVec2 pts[4] = {{cx, cy - half}, {cx + half, cy}, {cx, cy + half}, {cx - half, cy}};
            dl->AddConvexPolyFilled(pts, 4, color);
            return;
        }
        case IconShape::Triangle:
            dl->AddTriangleFilled({cx, cy - half}, {cx + half, cy + half * 0.8f}, {cx - half, cy + half * 0.8f}, color);
            return;
        case IconShape::Star: star(dl, cx, cy, size, color); return;
        case IconShape::Chevron:
            dl->PathLineTo({cx - half * 0.6f, cy - half * 0.5f});
            dl->PathLineTo({cx + half * 0.5f, cy});
            dl->PathLineTo({cx - half * 0.6f, cy + half * 0.5f});
            dl->PathStroke(color, ImDrawFlags_None, thickness + 0.5f);
            return;
        case IconShape::Microphone: microphone(dl, cx, cy, size, color, thickness); return;
        case IconShape::MicrophoneMuted:
            microphone(dl, cx, cy, size, color, thickness);
            slash(dl, cx, cy, size, color, thickness);
            return;
        case IconShape::Speaker: speaker(dl, cx, cy, size, color, thickness, true); return;
        case IconShape::SpeakerMuted:
            speaker(dl, cx, cy, size, color, thickness, false);
            slash(dl, cx, cy, size, color, thickness);
            return;
        case IconShape::Moon: moon(dl, cx, cy, size, color); return;
        case IconShape::Record:
            dl->AddCircleFilled({cx, cy}, half * 0.62f, color, 14);
            dl->AddCircle({cx, cy}, half * 0.92f, with_alpha(color, 0.55f), 16, thickness);
            return;
        case IconShape::Crown: crown(dl, cx, cy, size, color); return;
        case IconShape::Whisper: whisper(dl, cx, cy, size, color); return;
        case IconShape::Bars: bars(dl, cx, cy, size, color); return;
        default: return;
    }
}

void panel(ImDrawList* dl, float x, float y, float w, float h, float rounding, Color fill, Color border,
           float border_thickness) {
    if (!dl || w <= 0.f || h <= 0.f) return;
    if (fill >> 24) dl->AddRectFilled({x, y}, {x + w, y + h}, fill, rounding);
    if ((border >> 24) && border_thickness > 0.f)
        dl->AddRect({x, y}, {x + w, y + h}, border, rounding, ImDrawFlags_None, border_thickness);
}

}  // namespace yap::icons
