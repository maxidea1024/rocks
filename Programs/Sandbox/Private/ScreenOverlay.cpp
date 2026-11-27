#include "ScreenOverlay.h"

#include <Core/Math/MathUtility.h>
#include <UiCore/Declarative.h>
#include <UiFramework/Effects/BackgroundBlur.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Layout/VerticalStack.h>

namespace sandbox::screen {

namespace {

// 창 대상의 clear 색이다(장면 위 Load라 쓰이지 않는다 — 계약상 값만 둔다).
constexpr LinearColor UnusedClear{0.0f, 0.0f, 0.0f, 1.0f};

} // namespace

uint32 OverlayScale(uint32 height) noexcept {
    const uint32 scale = height / OverlayReferenceHeight;
    return scale == 0 ? 1u : scale;
}

ScreenOverlay::ScreenOverlay(Allocator& allocator) noexcept
    : allocator_(allocator), app_(allocator), batch_(allocator) {
}

ScreenOverlay::~ScreenOverlay() noexcept {
    app_.SetRoot({});
    root_ = {};
    for (SharedPtr<ui::ProgressBar>& bar : bars_) {
        bar = {};
    }

    app_.Shutdown();
}

Result<void> ScreenOverlay::Create() noexcept {
    if (root_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ui::ElementBuildScope scope(allocator_);
    for (SharedPtr<ui::ProgressBar>& bar : bars_) {
        auto made = ui::Build<ui::ProgressBar>(
            {.desiredSize = Vec2(0.0f, OverlayBarHeight), .value = 0.0f});
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        bar = made.Value().AsSharedPtr();
    }

    const ui::StackPanel::SlotArgs rows[OverlayBarCount] = {
        {.padding = OverlaySlotPadding, .content = bars_[0]},
        {.padding = OverlaySlotPadding, .content = bars_[1]},
        {.padding = OverlaySlotPadding, .content = bars_[2]}};
    const LinearColor panelColor{OverlayPanelColor[0], OverlayPanelColor[1], OverlayPanelColor[2],
                                 OverlayPanelColor[3]};
    ui::CanvasSlot slot;
    // 왼쪽 아래 기준점에 패널 왼쪽 아래 모서리를 둔다(위로 올리는 offset은 음수 y).
    slot.anchorMin = Vec2(0.0f, 1.0f);
    slot.anchorMax = Vec2(0.0f, 1.0f);
    slot.alignment = Vec2(0.0f, 1.0f);
    slot.offset = Vec4(OverlayMargin, -OverlayMargin, OverlayWidth, OverlayHeight);
    const ui::AnchorCanvas::SlotArgs panels[] = {
        {.layout = slot,
         .content =
             ui::Make<ui::Frame>({.content = ui::Make<ui::BackgroundBlur>(
                                      {.content = ui::Make<ui::VerticalStack>({.slots = rows}),
                                       .strength = OverlayBlurStrength}),
                                  .color = panelColor,
                                  .padding = Vec4(OverlayFramePadding, OverlayFramePadding,
                                                  OverlayFramePadding, OverlayFramePadding)})}};
    auto root = ui::Build<ui::AnchorCanvas>({.slots = panels});
    if (!root) {
        return Result<void>::Failure(root.Error());
    }

    root_ = root.Value().AsSharedPtr();
    app_.SetRoot(root_);
    return Result<void>::Success();
}

Result<void> ScreenOverlay::Record(const OverlayState& state, ui::UiWindowSurfaceId window,
                                   uint32 width, uint32 height, ui::UiDrawBuffer& buffer) noexcept {
    if (!root_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (width == 0 || height == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const float values[OverlayBarCount] = {state.entry, state.mip, state.layer};
    for (uint32 index = 0; index < OverlayBarCount; ++index) {
        if (auto set = bars_[index]->SetValue(math::Clamp(values[index], 0.0f, 1.0f)); !set) {
            return set;
        }
    }

    // 배율은 창 루트 변환으로 준다(UiDemo DockWindows와 같은 방식 — UiToolkit 19.2절). UI 좌표 =
    // client 픽셀 / 배율이다.
    const float scale = static_cast<float>(OverlayScale(height));
    const Vec2 viewport(static_cast<float>(width) / scale, static_cast<float>(height) / scale);
    auto cache =
        app_.UpdatePaintCache(ui::Geometry::Root(viewport, {.m00 = scale, .m11 = scale}), scale);
    if (!cache) {
        return Result<void>::Failure(cache.Error());
    }

    if (auto updated = batch_.Update(*cache.Value()); !updated) {
        return updated;
    }

    return buffer.AddWindow(window, width, height, UnusedClear, batch_);
}

} // namespace sandbox::screen
