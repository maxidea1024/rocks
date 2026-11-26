#include "DemoShaders.h"
#include "SurfaceSelfTest.h"
#include <Core/ScopeExit.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Effects/BackgroundBlur.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Effects/PostBufferUpdate.h>
#include <UiFramework/Effects/RetainerBox.h>
#include <UiFramework/Effects/Viewport.h>
#include <UiRender/DrawBatchBuffers.h>
#include <UiRender/WidgetSurfaceHost.h>

namespace rocks::ui {

int VerifyWidgetSurfaces(Allocator& allocator, graphics::Device& device,
                         graphics::PipelineHandle solid) noexcept {
    constexpr uint32 width = 16;
    constexpr uint32 height = 8;
    const auto bounds = Geometry::Root({width, height}, {});
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto retainer = SharedPtr<RetainerBox>::TryMake(allocator, allocator);
    auto viewport = SharedPtr<Viewport>::TryMake(allocator, allocator);
    auto blur = SharedPtr<BackgroundBlur>::TryMake(allocator, allocator);
    auto capture = SharedPtr<PostBufferUpdate>::TryMake(allocator, allocator);
    if (!root || !retainer || !viewport || !blur || !capture ||
        !root.Value()->TryAddChild(retainer.Value()) ||
        !retainer.Value()->TryAddChild(viewport.Value()) ||
        !root.Value()->TryAddChild(blur.Value()) || !root.Value()->TryAddChild(capture.Value()) ||
        !retainer.Value()->SetPhase(0, 2) || !blur.Value()->SetStrength(4)) {
        return 61;
    }

    Application app(allocator);
    app.SetRoot(root.Value());
    if (!app.PrepareLayout(bounds, 1)) {
        return 62;
    }

    SlotArray<BrushTexture> textures(allocator);
    WidgetSurfaceHost host(allocator, device, ActiveShaders(), textures);
    bool blue = false;
    host.SetScenePainter(WidgetSurfaceHost::ScenePainter::CreateLambda(
        [&](Element&, graphics::Device& gpu, graphics::TextureHandle target, uint32,
            uint32) noexcept -> Result<void> {
            PaintList list(allocator);
            auto added = list.TryAddBox({Geometry::Root({8, 8}, {}), {0, 1, 0, 1}, 0});
            DrawBatch batch(allocator);
            if (!added) {
                return added;
            }

            auto built = batch.Build(list);
            if (!built) {
                return built;
            }

            auto frame = gpu.BeginOffscreen(target, blue ? graphics::ClearColor{0, 0, 1, 1}
                                                         : graphics::ClearColor{1, 0, 0, 1});
            if (!frame) {
                return Result<void>::Failure(frame.Error());
            }

            auto drawn = DrawBatchRenderer::Draw(gpu, ActiveShaders(), *frame.Value(), solid, batch,
                                                 {width, height});
            auto ended = gpu.EndFrame();
            if (!drawn) {
                return drawn;
            }

            return ended ? Result<void>::Success() : Result<void>::Failure(ended.Error());
        }));
    Array<uint8> pixels(allocator);
    Array<uint8> copy(allocator);
    if (!copy.TryResize(width * height * 4)) {
        return 63;
    }

    for (uint32 frame = 0; frame < 3; ++frame) {
        blue = frame > 0;
        auto prepared = host.Prepare(root.Value(), frame, width, height, solid);
        auto cached = app.UpdateArrangedPaintCache();
        DrawBatch batch(allocator);
        DrawBatchBuffers buffers(device, ActiveShaders(), batch);
        if (!prepared || !cached || !batch.Update(*cached.Value()) || !buffers.Upload() ||
            !DrawBatchRenderer::ReadPixels(device, solid, buffers, textures, width, height,
                                           pixels)) {
            return 64;
        }

        const auto* target = textures.Find(capture.Value()->ManagedSurface());
        if (!target || !device.ReadPixels(target->texture, copy.View())) {
            return 65;
        }

        for (usize i = 0; i < pixels.Size(); ++i) {
            if (pixels[i] != copy[i]) {
                return 66;
            }
        }

        // 왼쪽 끝은 초록, 오른쪽 끝은 보관 주기(0, 2)에 맞춰 빨강에서 파랑으로 바뀐다.
        // 중앙 경계에는 필터가 두 색을 섞어야 한다.
        const auto row = width * 4 * 4;
        const auto right = row + (width - 1) * 4;
        const auto edge = row + 7 * 4;
        const uint32 channel = frame == 2 ? 2u : 0u;
        if (pixels[row + 1] < 245 || pixels[right + channel] < 245 ||
            pixels[right + (channel == 0 ? 2 : 0)] > 10 || pixels[edge + channel] < 10 ||
            pixels[edge + 1] < 10) {
            return 67;
        }
    }

    auto render = [&](uint32 frame) noexcept -> bool {
        auto prepared = host.Prepare(root.Value(), frame, width, height, solid);
        auto cached = app.UpdateArrangedPaintCache();
        DrawBatch batch(allocator);
        DrawBatchBuffers buffers(device, ActiveShaders(), batch);
        return prepared && cached && batch.Update(*cached.Value()) && buffers.Upload() &&
               DrawBatchRenderer::ReadPixels(device, solid, buffers, textures, width, height,
                                             pixels);
    };
    // 경계(4행 7열) 픽셀의 R 위치다. 흐림은 오른쪽의 파랑을 섞으므로 흐림 검사는 파랑 채널(+2)을
    // 본다 (효과 재질이 없어 채널을 바꾸지 않는다 — 효과 재질은 머티리얼 셰이더가 생기면 다시
    // 검사한다, UiToolkit 9.1절 UW1a).
    const uint32 edge = (4 * width + 7) * 4;
    constexpr uint32 blueChannel = 2;
    const uint32 edgeBlue = edge + blueChannel;
    if (!blur.Value()->SetStrength(0)) {
        return 68;
    }

    // 둥근 모서리는 원본 픽셀을 유지하고 직사각형의 같은 위치에는 블러가 적용된다.
    if (!blur.Value()->SetStrength(24) || !blur.Value()->SetCornerRadius({4, 4, 4, 4}) ||
        !render(4) || pixels[0] > 5 || pixels[1] < 245 || pixels[edgeBlue] < 10) {
        return 70;
    }

    if (!blur.Value()->SetCornerRadius({}) || !render(5) || pixels[blueChannel] < 10) {
        return 71;
    }

    if (!blur.Value()->SetRenderOpacity(0) || !render(6) || pixels[edgeBlue] > 5) {
        return 72;
    }

    blur.Value()->SetApplyAlphaToBlur(false);
    if (!render(7) || pixels[edgeBlue] < 10) {
        return 73;
    }

    Brush fallback;
    fallback.tint = {1, 0, 1, 1};
    if (!blur.Value()->SetRenderOpacity(1) || !blur.Value()->SetLowQualityFallbackBrush(fallback)) {
        return 74;
    }

    blur.Value()->SetLowQuality(true);
    if (!render(8) || pixels[edge] < 245 || pixels[edge + 2] < 245 || pixels[edge + 1] > 5) {
        return 75;
    }

    // 같은 화면을 두 목적지에 기록한다. 필터가 대상에만 적용되고 원본이 복원되어야 한다.
    blur.Value()->SetLowQuality(false);
    if (!blur.Value()->SetStrength(0)) {
        return 76;
    }

    graphics::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto processed = device.CreateTexture(desc, {});
    if (!processed) {
        return 77;
    }

    ROCKS_ON_SCOPE_EXIT {
        device.Retire(processed.Value());
    };
    auto slot = textures.TryAdd({processed.Value(), {}});
    if (!slot) {
        return 78;
    }

    ROCKS_ON_SCOPE_EXIT {
        (void)textures.TryRemove(slot.Value());
    };
    const PostBufferUpdateInfo targets[] = {{slot.Value(), 4}, {{}, 0}};
    if (!capture.Value()->SetUpdates(targets) || !render(9) || pixels[edgeBlue] > 5 ||
        !device.ReadPixels(processed.Value(), copy.View()) || copy[edgeBlue] < 10) {
        return 79;
    }

    auto* plain = textures.Find(capture.Value()->ManagedSurface());
    if (!plain || !device.ReadPixels(plain->texture, copy.View())) {
        return 80;
    }

    for (usize i = 0; i < pixels.Size(); ++i) {
        if (pixels[i] != copy[i]) {
            return 81;
        }
    }

    // 번호로 지정한 채널은 별도 표면에 기록되고 후속 위젯이 같은 핸들을 얻는다.
    const PostBufferUpdateInfo channels[] = {{{}, 4, 1}, {{}, 0, 2}};
    if (!capture.Value()->SetUpdates(channels) || !render(10)) {
        return 82;
    }

    auto* filteredChannel = textures.Find(host.GetPostBuffer(1));
    auto* plainChannel = textures.Find(host.GetPostBuffer(2));
    if (!filteredChannel || !plainChannel ||
        host.GetPostBuffer(1) != capture.Value()->GetPostBuffer(1) ||
        !device.ReadPixels(filteredChannel->texture, copy.View()) || copy[edgeBlue] < 10 ||
        !device.ReadPixels(plainChannel->texture, copy.View()) || copy[edgeBlue] > 5) {
        return 83;
    }

    const int viewportCheck = VerifyViewportWorld(allocator, device, solid);
    return viewportCheck ? viewportCheck : VerifyWorldWidgetPlane(allocator, device);
}

} // namespace rocks::ui
