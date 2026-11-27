#include "DemoCapture.h"
#include "SceneBoxes.h"
#include "SurfaceSelfTest.h"

#include <Core/ScopeExit.h>
#include <UiCore/Rendering/SurfaceRecorder.h>
#include <UiCore/Rendering/ViewportSource.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Effects/BackgroundBlur.h>
#include <UiFramework/Effects/PostBufferUpdate.h>
#include <UiFramework/Effects/RetainerBox.h>
#include <UiFramework/Effects/Viewport.h>
#include <UiFramework/Layout/Overlay.h>

namespace rocks::ui {

namespace {

// 캡처 대상의 바탕색이다(이전 직접 읽기 경로와 같은 색).
constexpr LinearColor CaptureClear{0.025f, 0.03f, 0.04f, 1};

// 왼쪽 8×8에 초록 상자를 그리고 나머지는 바탕(첫 프레임 빨강, 그 뒤 파랑)인 장면이다.
class SplitScene final : public ViewportSource {
public:
    explicit SplitScene(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    Result<void> Advance(uint64, const FrameTime&) noexcept override {
        return Result<void>::Success();
    }

    LinearColor Background() const noexcept override {
        return blue ? LinearColor{0, 0, 1, 1} : LinearColor{1, 0, 0, 1};
    }

    Result<void> Extract(UiSceneExtract& extract, const ViewportTarget&) noexcept override {
        PaintList list(allocator_);
        if (auto added = list.TryAddBox({Geometry::Root({8, 8}, {}), {0, 1, 0, 1}, 0}); !added) {
            return added;
        }

        return PostSceneBoxes(extract, allocator_, list);
    }

    bool blue = false;

private:
    Allocator& allocator_;
};

} // namespace

int VerifyWidgetSurfaces(Allocator& allocator, LaneUiRenderer& renderer,
                         render::RenderLane& lane) noexcept {
    constexpr uint32 width = 16;
    constexpr uint32 height = 8;
    const auto bounds = Geometry::Root({width, height}, {});
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto retainer = SharedPtr<RetainerBox>::TryMake(allocator, allocator);
    auto viewport = SharedPtr<Viewport>::TryMake(allocator, allocator);
    auto blur = SharedPtr<BackgroundBlur>::TryMake(allocator, allocator);
    auto capture = SharedPtr<PostBufferUpdate>::TryMake(allocator, allocator);
    auto scene = SharedPtr<SplitScene>::TryMake(allocator, allocator);
    if (!root || !retainer || !viewport || !blur || !capture || !scene ||
        !root.Value()->TryAddChild(retainer.Value()) ||
        !retainer.Value()->TryAddChild(viewport.Value()) ||
        !root.Value()->TryAddChild(blur.Value()) || !root.Value()->TryAddChild(capture.Value()) ||
        !retainer.Value()->SetPhase(0, 2) || !blur.Value()->SetStrength(4)) {
        return 61;
    }

    viewport.Value()->SetSceneSource(scene.Value());
    Application app(allocator);
    app.SetRoot(root.Value());
    if (!app.PrepareLayout(bounds, 1)) {
        return 62;
    }

    auto screen = renderer.CreateRenderTarget(width, height, UiTextureSampling::Image);
    if (!screen) {
        return 63;
    }

    ROCKS_ON_SCOPE_EXIT {
        renderer.ReleaseTexture(screen.Value());
    };
    SurfaceRecorder recorder(allocator, renderer);
    UiDrawBuffer buffer(allocator);
    DrawBatch batch(allocator);
    Array<uint8> pixels(allocator);
    Array<uint8> copy(allocator);
    // 표면 pass를 쌓고 창 내용을 캡처 대상에 그려 읽는다(Docs/Design/UiToolkit 18.5절).
    auto render = [&](uint64 frame) noexcept -> bool {
        auto recorded = recorder.Record(root.Value(), frame, width, height, buffer);
        auto cached = app.UpdateArrangedPaintCache();
        return recorded && cached && batch.Update(*cached.Value()) &&
               DrawAndCapture(renderer, buffer, screen.Value(), width, height, CaptureClear, batch,
                              pixels);
    };
    const auto read = [&](UiTextureId texture) noexcept {
        return !texture.IsNull() && CaptureTarget(renderer, texture, copy).IsOk();
    };
    for (uint32 frame = 0; frame < 3; ++frame) {
        scene.Value()->blue = frame > 0;
        if (!render(frame)) {
            return 64;
        }

        // 그리기 순서 끝의 화면 복사(PostBufferUpdate)는 창 결과와 같다.
        if (!read(capture.Value()->ManagedSurface())) {
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

    // 경계(4행 7열) 픽셀의 R 위치다. 흐림은 오른쪽의 파랑을 섞으므로 흐림 검사는 파랑 채널(+2)을
    // 본다(효과 재질이 없어 채널을 바꾸지 않는다 — 효과 재질은 머티리얼 셰이더가 생기면 다시
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

    auto processed = renderer.CreateRenderTarget(width, height, UiTextureSampling::Image);
    if (!processed) {
        return 77;
    }

    ROCKS_ON_SCOPE_EXIT {
        renderer.ReleaseTexture(processed.Value());
    };
    const PostBufferUpdateInfo targets[] = {{processed.Value(), 4}, {{}, 0}};
    if (!capture.Value()->SetUpdates(targets) || !render(9) || pixels[edgeBlue] > 5 ||
        !read(processed.Value()) || copy[edgeBlue] < 10) {
        return 79;
    }

    if (!read(capture.Value()->ManagedSurface())) {
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

    if (recorder.GetPostBuffer(1) != capture.Value()->GetPostBuffer(1) ||
        !read(recorder.GetPostBuffer(1)) || copy[edgeBlue] < 10 ||
        !read(recorder.GetPostBuffer(2)) || copy[edgeBlue] > 5) {
        return 83;
    }

    const int viewportCheck = VerifyViewportWorld(allocator, renderer);
    return viewportCheck ? viewportCheck : VerifyWorldWidgetPlane(allocator, renderer, lane);
}

} // namespace rocks::ui
