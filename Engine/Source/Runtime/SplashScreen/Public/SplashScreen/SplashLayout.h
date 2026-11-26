#pragma once

#include <SplashScreen/SplashImage.h>

// 글자 칸과 진행 막대의 배치다(Docs/Design/SplashScreen 6.2절). 위치는 논리 px로 이미지의 아래·좌우
// 가장자리 기준이고, ComputeSplashLayout이 창 px(배율 적용)로 바꾼다. 이미지는 아래
// SplashTextBandHeight 논리 px를 어둡고 고요한 글자 띠로 둔다(기본 이미지가 지킨다).
namespace rocks::splash {

// 글자 띠의 높이(논리 px)다.
inline constexpr int32 SplashTextBandHeight = 96;

// 좌우 여백(논리 px)이다.
inline constexpr int32 SplashSideMargin = 32;

// 칸·막대의 기본 위치(이미지 아래 가장자리에서 위쪽 가장자리까지, 논리 px)와 높이다.
inline constexpr int32 SplashTitleFromBottom = 84;
inline constexpr int32 SplashTitleHeight = 28;
inline constexpr int32 SplashVersionFromBottom = 55;
inline constexpr int32 SplashVersionHeight = 18;
inline constexpr int32 SplashProgressFromBottom = 34;
inline constexpr int32 SplashProgressHeight = 18;
inline constexpr int32 SplashCopyrightFromBottom = 33;
inline constexpr int32 SplashCopyrightHeight = 16;
inline constexpr int32 SplashBarFromBottom = 12;
inline constexpr int32 SplashBarHeight = 2;

// 진행 문구 칸과 저작권 칸을 가르는 가로 위치(논리 폭에 대한 비율)와 그 사이 틈(논리 px)이다.
inline constexpr double SplashProgressWidthRatio = 0.62;
inline constexpr int32 SplashColumnGap = 8;

// 글꼴 높이(논리 px, em 높이)다.
inline constexpr double SplashTitleFontPixels = 20.0;
inline constexpr double SplashNormalFontPixels = 12.0;
inline constexpr double SplashSmallFontPixels = 11.0;

// 기본 색(sRGB 0xRRGGBB)이다. 막대 바탕은 흰색을 SplashBarTrackAlpha만큼 섞는다.
inline constexpr uint32 SplashTitleColor = 0xFFFFFF;
inline constexpr uint32 SplashVersionColor = 0xA8A8A8;
inline constexpr uint32 SplashProgressColor = 0xD6D6D6;
inline constexpr uint32 SplashCopyrightColor = 0x808080;
inline constexpr uint32 SplashBarFillColor = 0xF5F2EB; // Warm Ivory
inline constexpr uint32 SplashBarTrackColor = 0xFFFFFF;
inline constexpr double SplashBarTrackAlpha = 0.12;
inline constexpr uint32 SplashBackgroundColor = 0x111111; // Rocks Black — 이미지 alpha 합성 바탕

// 불확정 막대에서 지나가는 조각의 폭(막대 폭에 대한 비율)이다.
inline constexpr double SplashMarqueeWidthRatio = 0.2;

// 창 px 사각형이다(right·bottom은 끝 다음).
struct SplashRect {
    int32 left = 0;
    int32 top = 0;
    int32 right = 0;
    int32 bottom = 0;

    friend constexpr bool operator==(const SplashRect&, const SplashRect&) noexcept = default;
};

enum class SplashTextAlign : uint8 {
    Left,
    Right,
};

// 칸 하나의 그리기 모양이다.
struct SplashTextStyle {
    // 글꼴 em 높이(창 px)다.
    int32 fontPixels = 0;
    bool semibold = false;
    uint32 color = 0;
    SplashTextAlign align = SplashTextAlign::Left;
};

// 창 px로 바꾼 배치다.
struct SplashLayout {
    SplashSize window;
    SplashRect text[SplashTextSlotCount];
    SplashTextStyle style[SplashTextSlotCount];
    SplashRect bar;
};

// 논리 크기와 배율로 기본 배치를 창 px로 계산한다. 칸 사각형은 창 안으로 잘린다.
//
// 사용 예:
//   const splash::SplashLayout layout = splash::ComputeSplashLayout({768, 432}, 1.5);
//   const splash::SplashRect title = layout.text[0];   // SplashTextSlot::Title
ROCKS_SPLASHSCREEN_API SplashLayout ComputeSplashLayout(SplashSize logical, double scale) noexcept;

} // namespace rocks::splash
