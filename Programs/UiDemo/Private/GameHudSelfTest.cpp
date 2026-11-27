#include "DemoCapture.h"
#include "GameHud.h"

#include <Core/Diagnostics/Log.h>
#include <Core/ScopeExit.h>
#include <UiFramework/Application/Application.h>

#include <UiWidget/ProgressBarWidget.generated.h>

namespace rocks::ui {

namespace {

inline constexpr LogCategory LogHudSelfTest{"UiDemo.HudSelfTest"};

} // namespace

int RunGameHudSelfTest(Allocator& allocator, UiRenderer& renderer) noexcept {
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
    // 소멸 전에 루트를 떼고 HUD Element를 놓은 뒤 미룬 해체 통지를 돌린다(Docs/Design/UiToolkit
    // 21.4절 — 앱이 먼저 부서지면 OnDestruct를 전할 곳이 없다).
    ROCKS_ON_SCOPE_EXIT {
        app.SetRoot({});
        hud.Value().root = {};
        app.Shutdown();
    };
    app.SetRoot(hud.Value().root);
    auto target = renderer.CreateRenderTarget(64, 16, UiTextureSampling::Image);
    if (!target) {
        return 4;
    }

    ROCKS_ON_SCOPE_EXIT {
        renderer.ReleaseTexture(target.Value());
    };
    DrawBatch batch(allocator);
    UiDrawBuffer buffer(allocator);
    Array<uint8> captured(allocator);
    auto bar =
        objects.ResolveForWrite<ProgressBarWidget>(objects.OwnerAccess(), hud.Value().progress);
    if (!bar) {
        return 5;
    }

    const auto viewport = Geometry::Root({64, 16}, {});
    // 가운데 픽셀은 처음에는 바탕(UE "ProgressBar" Foldout — 회색), 다음에는 채움(PrimaryHover —
    // 파랑)이다. 두 번째 batch는 정점만 부분 갱신한다.
    uint8 background[4]{};
    constexpr float fractions[] = {0.25f, 0.75f};
    for (const float fraction : fractions) {
        if (!bar.Value().Get().Value()->SetValue(fraction)) {
            return 6;
        }

        auto cache = app.UpdatePaintCache(viewport, 1);
        if (!cache || !batch.Update(*cache.Value())) {
            return 7;
        }

        if (!DrawAndCapture(renderer, buffer, target.Value(), 64, 16, {0, 0, 0, 1}, batch,
                            captured) ||
            captured.Size() != 64 * 16 * 4) {
            return 9;
        }

        const usize center = (8 * 64 + 32) * 4;
        const uint8* pixel = captured.Data() + center;
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

    return 0;
}

} // namespace rocks::ui
