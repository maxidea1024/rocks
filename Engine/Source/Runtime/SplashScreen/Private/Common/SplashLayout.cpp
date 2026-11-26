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

} // namespace

SplashLayout ComputeSplashLayout(SplashSize logical, double scale) noexcept {
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
        layout.text[index] = ScaleRect(place.left, height - place.fromBottom, right - place.left,
                                       place.lineHeight, effective, layout.window);
        SplashTextStyle& style = layout.style[index];
        style.fontPixels = ScaleLength(place.fontPixels, effective);
        style.semibold = place.semibold;
        style.color = place.color;
        style.align = place.align;
    }

    layout.bar = ScaleRect(margin, height - SplashBarFromBottom, contentRight - margin,
                           SplashBarHeight, effective, layout.window);
    return layout;
}

} // namespace rocks::splash
