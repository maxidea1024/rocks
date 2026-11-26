#include "../SplashPresenter.h"

#include <Platform/PlatformMath.h>
#include <SplashScreen/SplashLayout.h>

namespace rocks::splash {

namespace {

// 논리 px 값을 창 px로 반올림한다.
int32 ScaleLength(double value, double scale) noexcept {
    return static_cast<int32>(platform::PlatformMath::Round(value * scale));
}

int32 ClampToRange(int32 value, int32 low, int32 high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

// 논리 사각형(왼쪽·위·폭·높이)을 창 px로 바꾸고 창 안으로 자른다.
SplashRect ScaleRect(double left, double top, double width, double height, double scale,
                     SplashSize window) noexcept {
    const int32 maxX = static_cast<int32>(window.width);
    const int32 maxY = static_cast<int32>(window.height);
    SplashRect rect;
    rect.left = ClampToRange(ScaleLength(left, scale), 0, maxX);
    rect.top = ClampToRange(ScaleLength(top, scale), 0, maxY);
    rect.right = ClampToRange(ScaleLength(left + width, scale), rect.left, maxX);
    rect.bottom = ClampToRange(ScaleLength(top + height, scale), rect.top, maxY);
    return rect;
}

bool InRange(int32 value, int32 low, int32 high) noexcept {
    return value >= low && value <= high;
}

bool InRange(double value, double low, double high) noexcept {
    return platform::PlatformMath::IsFinite(value) && value >= low && value <= high;
}

constexpr int32 MaxPosition = static_cast<int32>(MaxSplashImageDimension);

} // namespace

Result<void> ValidateSplashPlacements(Span<const SplashTextPlacement> placements,
                                      const SplashBarPlacement* bar) noexcept {
    bool seen[SplashTextSlotCount]{};
    for (const SplashTextPlacement& placement : placements) {
        const usize index = SlotIndex(placement.slot);
        if (index >= SplashTextSlotCount || seen[index] || !InRange(placement.x, 0, MaxPosition) ||
            !InRange(placement.y, 0, MaxPosition) || !InRange(placement.width, 0, MaxPosition) ||
            !InRange(placement.height, 1, MaxSplashTextHeight) ||
            !InRange(placement.fontPixels, MinSplashFontPixels, MaxSplashFontPixels) ||
            placement.color > MaxSplashColor) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        seen[index] = true;
    }

    if (bar != nullptr &&
        (!InRange(bar->margin, 0, MaxPosition / 2) || !InRange(bar->fromBottom, 0, MaxPosition) ||
         !InRange(bar->height, 1, MaxSplashBarHeight) || bar->trackColor > MaxSplashColor ||
         bar->fillColor > MaxSplashColor || !InRange(bar->trackAlpha, 0.0, 1.0))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

SplashLayout ComputeSplashLayout(SplashSize logical, double scale,
                                 Span<const SplashTextPlacement> placements,
                                 const SplashBarPlacement* bar) noexcept {
    SplashLayout layout;
    layout.window = SplashWindowSize(logical, scale);
    // SplashWindowSize와 같은 배율 보정을 쓰도록 실제 배율을 다시 계산한다.
    const double effective =
        logical.width == 0 ? 1.0 : static_cast<double>(layout.window.width) / logical.width;
    const double width = logical.width;
    const double height = logical.height;
    const double margin = SplashSideMargin;
    const double contentRight = width - margin;
    const double progressRight = width * SplashProgressWidthRatio;

    struct Place {
        SplashTextSlot slot;
        double left;
        double right;
        int32 fromBottom;
        int32 lineHeight;
        double fontPixels;
        bool semibold;
        uint32 color;
        SplashTextAlign align;
    };
    const Place places[] = {
        {SplashTextSlot::Title, margin, contentRight, SplashTitleFromBottom, SplashTitleHeight,
         SplashTitleFontPixels, true, SplashTitleColor, SplashTextAlign::Left},
        {SplashTextSlot::Version, margin, contentRight, SplashVersionFromBottom,
         SplashVersionHeight, SplashNormalFontPixels, false, SplashVersionColor,
         SplashTextAlign::Left},
        {SplashTextSlot::Progress, margin, progressRight, SplashProgressFromBottom,
         SplashProgressHeight, SplashNormalFontPixels, false, SplashProgressColor,
         SplashTextAlign::Left},
        {SplashTextSlot::Copyright, progressRight + SplashColumnGap, contentRight,
         SplashCopyrightFromBottom, SplashCopyrightHeight, SplashSmallFontPixels, false,
         SplashCopyrightColor, SplashTextAlign::Right},
    };
    for (const Place& place : places) {
        const usize index = SlotIndex(place.slot);
        const double right = place.right < place.left ? place.left : place.right;
        layout.placed[index] = true;
        layout.text[index] = ScaleRect(place.left, height - place.fromBottom, right - place.left,
                                       place.lineHeight, effective, layout.window);
        SplashTextStyle& style = layout.style[index];
        style.fontPixels = ScaleLength(place.fontPixels, effective);
        style.semibold = place.semibold;
        style.color = place.color;
        style.align = place.align;
    }

    // 덮어쓰기(6.5절): 기준 모서리에서 논리 사각형을 구해 같은 방식으로 바꾼다.
    for (const SplashTextPlacement& placement : placements) {
        const usize index = SlotIndex(placement.slot);
        if (index >= SplashTextSlotCount) {
            continue;
        }

        const bool fromRight = placement.anchor == SplashTextAnchor::BottomRight ||
                               placement.anchor == SplashTextAnchor::TopRight;
        const bool fromTop = placement.anchor == SplashTextAnchor::TopLeft ||
                             placement.anchor == SplashTextAnchor::TopRight;
        const double span = placement.width != 0 ? placement.width : width - margin - placement.x;
        const double rectWidth = span < 0.0 ? 0.0 : span;
        const double left = fromRight ? width - placement.x - rectWidth : placement.x;
        const double top = fromTop ? placement.y : height - placement.y;
        layout.placed[index] = true;
        layout.text[index] =
            ScaleRect(left, top, rectWidth, placement.height, effective, layout.window);
        SplashTextStyle& style = layout.style[index];
        style.fontPixels = ScaleLength(placement.fontPixels, effective);
        style.semibold = placement.semibold;
        style.color = placement.color;
        style.align = placement.align;
    }

    const SplashBarPlacement defaults;
    const SplashBarPlacement& barPlace = bar != nullptr ? *bar : defaults;
    const double barWidth = width - 2.0 * barPlace.margin;
    layout.bar =
        ScaleRect(barPlace.margin, height - barPlace.fromBottom, barWidth < 0.0 ? 0.0 : barWidth,
                  barPlace.height, effective, layout.window);
    layout.barTrackColor = barPlace.trackColor;
    layout.barTrackAlpha = barPlace.trackAlpha;
    layout.barFillColor = barPlace.fillColor;
    return layout;
}

} // namespace rocks::splash
