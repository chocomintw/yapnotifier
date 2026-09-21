#include "colorpicker.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "config.h"

namespace yap::colorpicker {
namespace {

// Drawn in the element's border box. Header: swatch + hex text; expanded: SV square on
// the left, hue and alpha bars on the right. All px, so dp scaling comes for free.
constexpr float kHeader = 18.f, kGap = 4.f, kBar = 14.f, kClear = 12.f;

Rml::ColourbPremultiplied pm(float r, float g, float b, float a = 1.f) {
    auto ch = [](float v) { return static_cast<Rml::byte>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return Rml::Colourb(ch(r), ch(g), ch(b), ch(a)).ToPremultiplied();
}

void rgb_from_hsv(float h, float s, float v, float& r, float& g, float& b) {
    const float c = v * s, x = c * (1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f)), m = v - c;
    const int i = static_cast<int>(h * 6.f) % 6;
    const float t[6][3] = {{c, x, 0}, {x, c, 0}, {0, c, x}, {0, x, c}, {x, 0, c}, {c, 0, x}};
    r = t[i][0] + m;
    g = t[i][1] + m;
    b = t[i][2] + m;
}

void hsv_from_rgb(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0.f ? d / mx : 0.f;
    if (d <= 0.f) return;  // keep the previous hue for greys
    if (mx == r) h = std::fmod((g - b) / d + 6.f, 6.f) / 6.f;
    else if (mx == g) h = ((b - r) / d + 2.f) / 6.f;
    else h = ((r - g) / d + 4.f) / 6.f;
}

// Quad with one colour per corner (tl, tr, br, bl): gradients come from interpolation.
void quad(Rml::Mesh& m, float x, float y, float w, float h, Rml::ColourbPremultiplied tl, Rml::ColourbPremultiplied tr,
          Rml::ColourbPremultiplied br, Rml::ColourbPremultiplied bl) {
    const int i = static_cast<int>(m.vertices.size());
    m.vertices.push_back({{x, y}, tl, {0, 0}});
    m.vertices.push_back({{x + w, y}, tr, {0, 0}});
    m.vertices.push_back({{x + w, y + h}, br, {0, 0}});
    m.vertices.push_back({{x, y + h}, bl, {0, 0}});
    for (int k : {0, 2, 1, 0, 3, 2}) m.indices.push_back(i + k);
}
void quad(Rml::Mesh& m, float x, float y, float w, float h, Rml::ColourbPremultiplied c) { quad(m, x, y, w, h, c, c, c, c); }

void checker(Rml::Mesh& m, float x, float y, float w, float h, float cell) {
    const auto a = pm(0.55f, 0.55f, 0.55f), b = pm(0.3f, 0.3f, 0.3f);
    quad(m, x, y, w, h, a);
    for (float cy = 0; cy < h; cy += cell)
        for (float cx = 0; cx < w; cx += cell)
            if ((static_cast<int>(cx / cell) + static_cast<int>(cy / cell)) % 2)
                quad(m, x + cx, y + cy, std::min(cell, w - cx), std::min(cell, h - cy), b);
}

class ElementColorPicker final : public Rml::Element {
public:
    explicit ElementColorPicker(const Rml::String& tag) : Rml::Element(tag) {}

protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override {
        Rml::Element::OnAttributeChange(changed);
        if (changed.count("value")) {
            parse(GetAttribute<Rml::String>("value", ""));
            dirty_ = true;
        }
        if (changed.count("optional")) optional_ = HasAttribute("optional");
    }
    void OnResize() override { dirty_ = true; }
    void OnPropertyChange(const Rml::PropertyIdSet& changed) override {
        Rml::Element::OnPropertyChange(changed);
        if (changed.Contains(Rml::PropertyId::Opacity)) dirty_ = true;
    }

    void OnRender() override {
        if (dirty_) rebuild();
        if (geometry_) geometry_.Render(GetAbsoluteOffset(Rml::BoxArea::Border));
    }

    void ProcessDefaultAction(Rml::Event& ev) override {
        Rml::Element::ProcessDefaultAction(ev);
        const Rml::Vector2f p = Rml::Vector2f(ev.GetParameter<float>("mouse_x", 0.f), ev.GetParameter<float>("mouse_y", 0.f)) -
                                GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Border);
        switch (ev.GetId()) {
            case Rml::EventId::Mousedown:
                if (ev.GetParameter<int>("button", 0) != 0) return;
                drag_ = Region::None;
                if (p.y < kHeader) {
                    if (expanded_ && optional_ && p.x >= size.x - kClear) {  // the "x": back to unset
                        set_unset(true);
                        expand(false);
                    } else if (optional_ && unset_) {
                        set_unset(false);
                        expand(true);
                    } else {
                        expand(!expanded_);
                    }
                    return;
                }
                if (!expanded_) return;
                drag_ = region(p, size);
                apply(p, size);
                break;
            case Rml::EventId::Drag:
                if (drag_ != Region::None) apply(p, size);
                break;
            case Rml::EventId::Dragend:
            case Rml::EventId::Mouseup:
                drag_ = Region::None;
                break;
            default:
                break;
        }
    }

private:
    enum class Region { None, SV, Hue, Alpha };

    // Geometry of the expanded part; `size` is the border box.
    float sv_side(Rml::Vector2f size) const { return std::max(0.f, std::min(size.y - kHeader - kGap, size.x - 2 * (kBar + kGap))); }
    Region region(Rml::Vector2f p, Rml::Vector2f size) const {
        const float side = sv_side(size);
        if (p.x < side) return Region::SV;
        if (p.x < side + kGap + kBar) return Region::Hue;
        return Region::Alpha;
    }
    void apply(Rml::Vector2f p, Rml::Vector2f size) {
        const float side = sv_side(size);
        if (side <= 0.f) return;
        const float fy = std::clamp((p.y - kHeader - kGap) / side, 0.f, 1.f);
        switch (drag_) {
            case Region::SV:
                s_ = std::clamp(p.x / side, 0.f, 1.f);
                v_ = 1.f - fy;
                break;
            case Region::Hue: h_ = fy; break;
            case Region::Alpha: a_ = 1.f - fy; break;
            default: return;
        }
        unset_ = false;
        publish();
    }

    void expand(bool on) {
        expanded_ = on;
        SetClass("expanded", on);
        dirty_ = true;
    }
    void set_unset(bool on) {
        unset_ = on;
        if (!on && a_ <= 0.f) a_ = 1.f;
        publish();
    }

    void parse(const Rml::String& text) {
        Color c = 0;
        if (!text.empty() && text[0] == '#') {
            c = config::parse_color(text);
            hex_ = true;
        } else {
            c = static_cast<Color>(std::strtoul(text.c_str(), nullptr, 10));
            hex_ = false;
        }
        unset_ = c == 0;
        if (unset_) return;
        const float r = static_cast<float>(c & 0xFF) / 255.f, g = static_cast<float>((c >> 8) & 0xFF) / 255.f,
                    b = static_cast<float>((c >> 16) & 0xFF) / 255.f;
        a_ = static_cast<float>(c >> 24) / 255.f;
        hsv_from_rgb(r, g, b, h_, s_, v_);
    }

    Color color() const {
        if (unset_) return 0;
        float r, g, b;
        rgb_from_hsv(h_, s_, v_, r, g, b);
        auto ch = [](float x) { return static_cast<unsigned>(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
        Color c = rgba(ch(r), ch(g), ch(b), ch(a_));
        return c == 0 ? rgba(0, 0, 0, 1) : c;  // keep "set" distinct from "unset"
    }

    // Writes the attribute and tells data-value; hex in, hex out (else the packed number).
    void publish() {
        const Color c = color();
        const Rml::String text = hex_ ? config::format_color(c) : std::to_string(c);
        SetAttribute("value", text);
        Rml::Dictionary params;
        params["value"] = Rml::Variant(text);
        DispatchEvent(Rml::EventId::Change, params);
        dirty_ = true;
    }

    void rebuild() {
        dirty_ = false;
        Rml::RenderManager* rm = GetRenderManager();
        if (!rm) return;
        const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Border);
        const float opacity = GetProperty<float>("opacity");
        Rml::Mesh m;
        float r = 1.f, g = 1.f, b = 1.f;
        rgb_from_hsv(h_, s_, v_, r, g, b);

        // header swatch (alpha over a checker), slashed when unset
        const float sw = std::min(size.x, 28.f);
        checker(m, 0, 0, sw, kHeader, 6.f);
        if (unset_) {
            quad(m, 0, 0, sw, kHeader, pm(0.12f, 0.13f, 0.15f, 0.9f * opacity));
            quad(m, 3, kHeader * 0.5f - 1.f, sw - 6.f, 2.f, pm(0.8f, 0.3f, 0.3f, opacity));
        } else {
            quad(m, 0, 0, sw, kHeader, pm(r, g, b, a_ * opacity));
        }
        quad(m, 0, 0, sw, 1.f, pm(0.8f, 0.8f, 0.9f, 0.5f * opacity));
        quad(m, 0, kHeader - 1.f, sw, 1.f, pm(0.8f, 0.8f, 0.9f, 0.5f * opacity));

        if (expanded_) {
            if (optional_) {  // the "x" box
                const float x0 = size.x - kClear, y0 = (kHeader - kClear) * 0.5f;
                quad(m, x0, y0, kClear, kClear, pm(0.45f, 0.2f, 0.2f, opacity));
                quad(m, x0 + 3.f, y0 + kClear * 0.5f - 1.f, kClear - 6.f, 2.f, pm(1, 1, 1, opacity));
            }
            const float side = sv_side(size), top = kHeader + kGap;
            if (side > 0.f) {
                float hr, hg, hb;
                rgb_from_hsv(h_, 1.f, 1.f, hr, hg, hb);
                // saturation left->right over white, value top->bottom to black (two blended quads)
                quad(m, 0, top, side, side, pm(1, 1, 1, opacity), pm(hr, hg, hb, opacity), pm(hr, hg, hb, opacity), pm(1, 1, 1, opacity));
                quad(m, 0, top, side, side, pm(0, 0, 0, 0), pm(0, 0, 0, 0), pm(0, 0, 0, opacity), pm(0, 0, 0, opacity));
                // cursor
                const float cx = s_ * side, cy = top + (1.f - v_) * side;
                quad(m, cx - 4.f, cy - 1.f, 8.f, 2.f, pm(1, 1, 1, opacity));
                quad(m, cx - 1.f, cy - 4.f, 2.f, 8.f, pm(1, 1, 1, opacity));
                quad(m, cx - 3.f, cy - 0.5f, 6.f, 1.f, pm(0, 0, 0, opacity));
                quad(m, cx - 0.5f, cy - 3.f, 1.f, 6.f, pm(0, 0, 0, opacity));

                // hue bar: six vertical segments through the spectrum
                const float hx = side + kGap;
                for (int i = 0; i < 6; ++i) {
                    float r0, g0, b0, r1, g1, b1;
                    rgb_from_hsv(static_cast<float>(i) / 6.f, 1, 1, r0, g0, b0);
                    rgb_from_hsv(static_cast<float>(i + 1) / 6.f - 0.0001f, 1, 1, r1, g1, b1);
                    const float y0 = top + side * static_cast<float>(i) / 6.f, hh = side / 6.f;
                    quad(m, hx, y0, kBar, hh, pm(r0, g0, b0, opacity), pm(r0, g0, b0, opacity), pm(r1, g1, b1, opacity), pm(r1, g1, b1, opacity));
                }
                quad(m, hx - 1.f, top + h_ * side - 1.f, kBar + 2.f, 2.f, pm(1, 1, 1, opacity));

                // alpha bar: colour fading to nothing over a checker
                const float ax = hx + kBar + kGap;
                checker(m, ax, top, kBar, side, 7.f);
                quad(m, ax, top, kBar, side, pm(r, g, b, opacity), pm(r, g, b, opacity), pm(r, g, b, 0), pm(r, g, b, 0));
                quad(m, ax - 1.f, top + (1.f - a_) * side - 1.f, kBar + 2.f, 2.f, pm(1, 1, 1, opacity));
            }
        }
        geometry_ = rm->MakeGeometry(std::move(m));
        SetInnerRML("<span class=\"hex\">" + (unset_ ? Rml::String("unset") : config::format_color(color())) + "</span>");
    }

    float h_ = 0.f, s_ = 0.f, v_ = 1.f, a_ = 1.f;
    bool unset_ = false, optional_ = false, expanded_ = false, hex_ = true, dirty_ = true;
    Region drag_ = Region::None;
    Rml::Geometry geometry_;
};

Rml::ElementInstancerGeneric<ElementColorPicker> g_instancer;

}  // namespace

void register_element() { Rml::Factory::RegisterElementInstancer("colorpicker", &g_instancer); }

}  // namespace yap::colorpicker
