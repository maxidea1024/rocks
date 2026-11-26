#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <SplashScreen/SplashImage.h>

// 글자 칸과 진행 막대의 배치다(Docs/Design/SplashScreen 6.2·6.5절). 위치는 논리 px로 이미지의
// 가장자리 기준이고, ComputeSplashLayout이 창 px(배율 적용)로 바꾼다. 이미지는 아래
// SplashTextBandHeight 논리 px를 어둡고 고요한 글자 띠로 둔다(기본 이미지가 지킨다). 호스트는
// SplashTextPlacement·SplashBarPlacement로 기본 배치를 덮어쓰고 Custom 칸을 둘 수 있다(코드 API —
// UE RegisterUserSplashText 대응, 설정 키는 없다).
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

// 덮어쓰기 값의 범위다(6.5절). 위치·폭은 0–MaxSplashImageDimension(창 밖은 잘린다), 칸 높이는
// 1–MaxSplashTextHeight, 글꼴은 MinSplashFontPixels–MaxSplashFontPixels, 막대 높이는
// 1–MaxSplashBarHeight, 색은 0xFFFFFF 이하, 막대 바탕 alpha는 0–1이다.
inline constexpr int32 MaxSplashTextHeight = 96;
inline constexpr double MinSplashFontPixels = 8.0;
inline constexpr double MaxSplashFontPixels = 48.0;
inline constexpr int32 MaxSplashBarHeight = 16;
inline constexpr uint32 MaxSplashColor = 0xFFFFFF;

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

// 덮어쓰기 위치의 기준 모서리다. x는 기준 쪽 좌우 가장자리에서, y는 Bottom 기준이면 아래
// 가장자리에서 사각형 위쪽까지, Top 기준이면 위 가장자리에서 사각형 위쪽까지 잰다.
enum class SplashTextAnchor : uint8 {
    BottomLeft,
    BottomRight,
    TopLeft,
    TopRight,
};

// 칸 하나의 배치 덮어쓰기다(논리 px). 칸 배치 전체를 정한다(빠진 값을 기본 배치에서 가져오지
// 않는다).
//
// 사용 예(Custom 칸 하나를 오른쪽 위에):
//   splash::SplashTextPlacement build;
//   build.slot = splash::SplashTextSlot::Custom1;
//   build.anchor = splash::SplashTextAnchor::TopRight;
//   build.x = 16;
//   build.y = 12;
//   build.width = 240;
//   build.height = 16;
//   build.align = splash::SplashTextAlign::Right;
//   desc.placements = Span<const splash::SplashTextPlacement>(&build, 1);
struct SplashTextPlacement {
    SplashTextSlot slot = SplashTextSlot::Title;
    SplashTextAnchor anchor = SplashTextAnchor::BottomLeft;
    int32 x = SplashSideMargin;
    int32 y = SplashTitleFromBottom;
    // 0이면 반대쪽 여백(SplashSideMargin)까지다.
    int32 width = 0;
    int32 height = SplashTitleHeight;
    double fontPixels = SplashNormalFontPixels;
    bool semibold = false;
    uint32 color = SplashTitleColor;
    SplashTextAlign align = SplashTextAlign::Left;
};

// 진행 막대의 배치 덮어쓰기다(논리 px).
struct SplashBarPlacement {
    // 좌우 여백이다.
    int32 margin = SplashSideMargin;
    // 아래 가장자리에서 막대 위쪽까지다.
    int32 fromBottom = SplashBarFromBottom;
    int32 height = SplashBarHeight;
    uint32 trackColor = SplashBarTrackColor;
    double trackAlpha = SplashBarTrackAlpha;
    uint32 fillColor = SplashBarFillColor;
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
    // 칸마다 배치가 있는지다(기본 칸은 늘 참, Custom 칸은 덮어쓰기를 받았을 때만).
    bool placed[SplashTextSlotCount]{};
    SplashRect text[SplashTextSlotCount];
    SplashTextStyle style[SplashTextSlotCount];
    SplashRect bar;
    uint32 barTrackColor = SplashBarTrackColor;
    double barTrackAlpha = SplashBarTrackAlpha;
    uint32 barFillColor = SplashBarFillColor;
};

// 덮어쓰기 값을 검사한다(6.5절 범위, 같은 칸 두 번). 오류: 범위 밖·NaN·중복 칸은 InvalidArgument.
// bar는 nullptr이어도 된다. SplashScreen::Show가 부른다.
ROCKS_SPLASHSCREEN_API Result<void>
ValidateSplashPlacements(Span<const SplashTextPlacement> placements,
                         const SplashBarPlacement* bar) noexcept;

// 논리 크기와 배율로 배치를 창 px로 계산한다. 기본 배치(6.2절) 위에 placements·bar를
// 덮어쓴다(검사를 통과한 값이어야 한다). 칸 사각형은 창 안으로 잘린다.
//
// 사용 예:
//   const splash::SplashLayout layout = splash::ComputeSplashLayout({768, 384}, 1.5);
//   const splash::SplashRect title = layout.text[0];   // SplashTextSlot::Title
ROCKS_SPLASHSCREEN_API SplashLayout ComputeSplashLayout(
    SplashSize logical, double scale, Span<const SplashTextPlacement> placements = {},
    const SplashBarPlacement* bar = nullptr) noexcept;

} // namespace rocks::splash
