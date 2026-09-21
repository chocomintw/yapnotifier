// Host-side check for the RmlUi documents: loads hud.rml / menu.rml through the real
// hud::init / menu::init (data models included) with a null renderer and fails on any
// RmlUi warning (bad RCSS, unknown data variable, broken template). Run from assets/ui.
#include <RmlUi/Core.h>

#include <cstdio>
#include <string>

#include "colorpicker.h"
#include "hud.h"
#include "menu.h"
#include "ui_files.h"

#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);     \
            return 1;                                                   \
        }                                                               \
    } while (0)

namespace {
int g_problems = 0;

struct NullRender : Rml::RenderInterface {
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dim, const Rml::String&) override { dim = {1, 1}; return 1; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

struct StrictSystem : Rml::SystemInterface {
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) {
            ++g_problems;
            std::printf("rmlui: %s\n", message.c_str());
        }
        return true;
    }
};
}  // namespace

int main() {
    NullRender render;
    StrictSystem system;
    Rml::SetRenderInterface(&render);
    Rml::SetSystemInterface(&system);
    Rml::SetFileInterface(&yap::ui_files::instance());  // no resources in an exe: falls through to disk
    CHECK(Rml::Initialise());
    CHECK(Rml::LoadFontFace("../Quicksand-Regular.ttf", "Yap", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal));
    yap::colorpicker::register_element();
    Rml::Context* ctx = Rml::CreateContext("test", {1920, 1080});
    CHECK(ctx);

    yap::Config cfg;
    cfg.chat_enabled = true;
    yap::hud::State hud;
    CHECK(yap::hud::init(*ctx));
    yap::menu::Host host{cfg, std::wstring(L"YapNotifier.ini"), true, hud};
    CHECK(yap::menu::init(host, *ctx));
    yap::menu::show(true);

    // A few frames of demo data through every anchor exercises each data-* binding.
    for (int frame = 0; frame < 30; ++frame) {
        cfg.anchor = static_cast<yap::Anchor>(frame % 9);
        cfg.notif_anchor = static_cast<yap::Anchor>((frame + 3) % 9);
        cfg.chat_anchor = static_cast<yap::Anchor>((frame + 6) % 9);
        const uint64_t now = 1000 + static_cast<uint64_t>(frame) * 3000;
        yap::hud::demo_tick(hud, cfg, now);
        yap::hud::sync(hud, cfg, yap::hud::demo_snapshot(), 16.f, now, frame < 5 ? "update notice" : "");
        yap::menu::sync(host);
        ctx->Update();
        ctx->Render();
    }
    CHECK(!hud.toasts.items().empty() || !hud.chat.empty());

    Rml::Shutdown();
    CHECK(g_problems == 0);
    std::printf("ok\n");
    return 0;
}
