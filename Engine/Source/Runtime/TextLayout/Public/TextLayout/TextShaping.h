#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <TextLayout/FontTypes.h>

// shaping·측정의 라이브러리 중립 형식이다(Docs/Design/Font 6.4·6.8절, UE
// FSlateTextShaper·FShapedGlyphSequence). 실제 shaping·경계·BiDi는 TextBackend·BackendFace가 하고,
// ShapeText(TextShaper.h)가 조합 글꼴과 묶는다.
namespace rocks::text {

class BackendFace;

// shaping 방식이다(UE ETextShapingMethod — 4.1절).
enum class TextShapingMethod : uint8 {
    Auto,
    KerningOnly,
    FullShaping,
};

// 문단 기본 방향이다(Auto는 첫 강한 방향 문자를 따른다).
enum class TextBaseDirection : uint8 {
    Auto,
    LeftToRight,
    RightToLeft,
};

// 경계 종류다(ICU BreakIterator 대응).
enum class TextBoundaryKind : uint8 {
    Grapheme,
    Word,
    Line,
    Sentence,
};

// 방향 run 하나다(UTF-8 byte 구간 — ResolveBidi는 시각 순서로 낸다).
struct BidiRun {
    usize start = 0;
    usize length = 0;
    bool rightToLeft = false;
};

// shaping한 글리프 하나다. cluster는 원문 UTF-8 byte 위치, 크기는 픽셀이다.
struct ShapedGlyph {
    BackendFace* face = nullptr;
    uint32 glyphIndex = 0;
    uint32 cluster = 0;
    float advance = 0;
    float offsetX = 0;
    float offsetY = 0;
};

// face 하나로 shaping할 run이다(BackendFace::Shape 입력).
struct ShapeRunRequest {
    // 같은 face로 그릴 UTF-8 run이다.
    StringView text = {};
    // text 첫 byte의 원문 위치다(ShapedGlyph::cluster = clusterBase + run 안 위치).
    uint32 clusterBase = 0;
    // 크기(96 DPI 포인트, 조합 글꼴 배율을 곱한 값)와 배율이다.
    float size = 0;
    float scale = 1.0f;
    bool rightToLeft = false;
    // FullShaping이 아니면 KerningOnly다(Auto는 호출자가 정한다).
    bool fullShaping = false;
    // 자간이 0이 아니면 true — 표준 합자(liga)를 끈다(6.4절).
    bool disableLigatures = false;
    // HarfBuzz 언어 태그(culture, 예 "ko-KR")다. 비면 추정한다.
    StringView language = {};
};

// UE Auto 판정의 문자 구간 표다: LTR이어도 결합 문자·ZWJ·복합 문자 script·BMP 밖 code point가
// 있으면 FullShaping이 필요하다(SlateTextShaper.cpp 154–273행). 잘못된 UTF-8은 true로 본다.
ROCKS_TEXTLAYOUT_API bool RequiresFullShaping(StringView utf8) noexcept;

// 글리프 advance의 합(픽셀)이다.
ROCKS_TEXTLAYOUT_API float MeasureWidth(Span<const ShapedGlyph> glyphs) noexcept;

} // namespace rocks::text
