#pragma once

#include <Core/Text/String.h>

// 글꼴 face·metric 형식이다(Docs/Design/Font 4.1·6.2·6.8절). 라이브러리(FreeType 등) 형식은 쓰지
// 않는다.
//
// 사용 예:
//   const uint32 pixels = text::ComputeFontPixelSize(24.0f, 1.0f); // 24pt @ 96 DPI → 32 px
namespace rocks::text {

// hinting 방식이다(UE EFontHinting과 같은 순서 — 4.1절). FreeType 플래그 대응은 6.2절 표다.
enum class FontHinting : uint8 {
    Default,
    Auto,
    AutoLight,
    Monochrome,
    None,
};

// 줄 높이를 글꼴 metric과 경계 상자 중 무엇으로 정하나다(UE EFontLayoutMethod).
enum class FontLayoutMethod : uint8 {
    Metrics,
    BoundingBox,
};

// 글꼴 크기 단위의 DPI다(크기는 이 DPI의 포인트 — 6.1절, UE FontConstants::RenderDPI).
inline constexpr uint32 FontRenderDpi = 96;

// 포인트 → 픽셀 환산의 기준 DPI(1 pt = 1/72 inch)다.
inline constexpr uint32 FontPointsPerInch = 72;

// 크기(96 DPI 포인트)와 배율에서 래스터 픽셀 크기를 낸다. UE ComputeFontPixelSize와 같은 고정소수
// 계산이다: 26.6로 바꾼 크기 × 96 / 72(반올림)에 16.16 배율을 곱해(반올림) 픽셀로 반올림한다.
// 음수는 0으로 본다.
ROCKS_TEXTLAYOUT_API uint32 ComputeFontPixelSize(float size, float scale) noexcept;

// face 하나의 정보다(편집기 하위 face 목록·검사용).
struct FontFaceInfo {
    explicit FontFaceInfo(Allocator& allocator) noexcept : family(allocator), style(allocator) {
    }

    String family;
    String style;
    uint32 glyphCount = 0;
    uint16 unitsPerEm = 0;
    // 윤곽 글꼴이면 true(비트맵 전용 글꼴은 false — 거리장을 켤 수 없다, 5.1절).
    bool scalable = false;
    // 컬러 글리프 표(CBDT·sbix·COLR)가 있으면 true.
    bool color = false;
    // 비트맵 strike 수다(컬러 비트맵 글꼴).
    int32 fixedSizeCount = 0;
};

// 크기·배율이 정해진 face의 줄 metric이다. 단위는 픽셀, 위쪽이 양수다(descender는 보통 음수).
struct FontMetrics {
    float ascender = 0;
    float descender = 0;
    float lineHeight = 0;
    float underlinePosition = 0;
    float underlineThickness = 0;
};

} // namespace rocks::text
