#include "DemoShaders.h"
#include "GameHud.h"

#include <Core/Diagnostics/Log.h>
#include <D3D11Graphics/D3D11Device.h>
#include <UiFramework/Application.h>
#include <UiRender/DrawBatchBuffers.h>
#include <UiRender/DrawBatchRenderer.h>

#include <UiWidget/ProgressBarWidget.generated.h>

namespace rocks::ui {

namespace {

inline constexpr LogCategory LogHudSelfTest{"UiDemo.HudSelfTest"};

} // namespace

int RunGameHudSelfTest(Allocator& allocator) noexcept {
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    if (!RegisterWidgetTypes(modules)) {
        return 1;
    }

    ObjectRegistry objects(allocator, types, {0x554954455354, 1});
    ObjectAccessScope scope(objects.OwnerAccess());
    auto hud = CreateGameHud(objects);
    if (!hud) {
        return 2;
    }

    Application app(allocator);
    app.SetRoot(hud.Value().root);
    auto created = graphics::CreateD3D11Device(allocator, {.preferSoftware = true});
    if (!created) {
        return 3;
    }

    auto& device = *created.Value();
    DemoShaderSet shaders(allocator, device);
    if (!shaders.IsReady()) {
        return 3;
    }

    graphics::TextureDesc desc;
    desc.width = 64;
    desc.height = 16;
    desc.format = graphics::PixelFormat::RGBA8Unorm;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto target = device.CreateTexture(desc, {});
    auto pipeline = DrawBatchRenderer::CreateSolidPipeline(device, ActiveShaders(), desc.format);
    if (!target || !pipeline) {
        return 4;
    }

    DrawBatch batch(allocator);
    DrawBatchBuffers buffers(device, ActiveShaders(), batch);
    auto bar =
        objects.ResolveForWrite<ProgressBarWidget>(objects.OwnerAccess(), hud.Value().progress);
    if (!bar) {
        return 5;
    }

    const auto viewport = Geometry::Root({64, 16}, {});
    uint8 pixels[64 * 16 * 4]{};
    // 가운데 픽셀은 처음에는 바탕(UE "ProgressBar" Foldout — 회색), 다음에는 채움(PrimaryHover —
    // 파랑)이다. 두 번째 제출은 기존 버퍼를 부분 갱신한다.
    uint8 background[4]{};
    constexpr float fractions[] = {0.25f, 0.75f};
    for (const float fraction : fractions) {
        if (!bar.Value().Get().Value()->SetValue(fraction)) {
            return 6;
        }

        auto cache = app.UpdatePaintCache(viewport, 1);
        if (!cache || !batch.Update(*cache.Value()) || !buffers.Upload()) {
            return 7;
        }

        auto frame = device.BeginOffscreen(target.Value(), {0, 0, 0, 1});
        if (!frame) {
            return 8;
        }

        auto drawn =
            DrawBatchRenderer::Draw(*frame.Value(), pipeline.Value(), buffers, viewport.size);
        if (!device.EndFrame() || !drawn || !device.ReadPixels(target.Value(), pixels)) {
            return 9;
        }

        const usize center = (8 * 64 + 32) * 4;
        const uint8* pixel = pixels + center;
        const auto mismatch = [&]() noexcept {
            Log::Error(LogHudSelfTest, "Progress {Fraction} center pixel mismatch: {R} {G} {B} {A}",
                       fraction, pixel[0], pixel[1], pixel[2], pixel[3]);
            return 10;
        };
        if (pixel[3] != 255) {
            return mismatch();
        }

        if (fraction < 0.5f) {
            // 바탕은 무채색이다.
            if (pixel[0] != pixel[1] || pixel[1] != pixel[2]) {
                return mismatch();
            }

            for (usize channel = 0; channel < 4; ++channel) {
                background[channel] = pixel[channel];
            }
        } else if (pixel[2] <= pixel[0] || pixel[2] == background[2]) {
            // 채움은 바탕과 다르고 파란 쪽이다.
            return mismatch();
        }

        const uint64 revision = cache.Value()->Revision();
        if (!app.UpdatePaintCache(viewport, 1) || cache.Value()->Revision() != revision ||
            cache.Value()->LastUpdate().paintCalls != 0) {
            return 11;
        }
    }

    device.Retire(pipeline.Value());
    device.Retire(target.Value());
    return 0;
}

} // namespace rocks::ui
