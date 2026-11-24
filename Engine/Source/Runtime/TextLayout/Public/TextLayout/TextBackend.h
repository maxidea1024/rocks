#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <TextLayout/FontFaceData.h>
#include <TextLayout/FontTypes.h>
#include <TextLayout/GlyphRaster.h>
#include <TextLayout/TextShaping.h>

// 글꼴 라이브러리 backend 계약이다(Docs/Design/Font 3장·6.8절). 구현은 ClientOnly 모듈
// TextLayoutBackend(FreeType·HarfBuzz·ICU·msdfgen)가 하고, 호스트(UI 앱·도구·시험)가 만들어
// 주입한다. 서버 타깃에는 구현이 없으므로 이 계약을 쓰는 쪽은 backend 없이 Unsupported로 끝난다.
//
// 사용 예:
//   auto face = backend.OpenFace({.data = data, .hinting = text::FontHinting::Default});
//   auto metrics = face.Value()->Metrics(24.0f, 1.0f);
namespace rocks::text {

// 열 face의 기술이다(4.2절 FontFace 설정의 실행 쪽).
struct FontFaceDesc {
    SharedPtr<const FontFaceData> data = {};
    // TTC·OTC의 하위 face 번호다.
    int32 subFaceIndex = 0;
    FontHinting hinting = FontHinting::Default;
    FontLayoutMethod layoutMethod = FontLayoutMethod::Metrics;
    // ascent·descent 재정의(글꼴 단위)다. 재정의하면 줄 높이 = ascent − descent다.
    bool overrideAscent = false;
    int32 ascentOverride = 0;
    bool overrideDescent = false;
    int32 descentOverride = 0;
};

// 열린 face 하나다. 스레드 안전하지 않다 — 한 번에 한 호출자가 쓴다(6.2절 face 잠금은 호출자 몫).
class ROCKS_TEXTLAYOUT_API BackendFace {
public:
    virtual ~BackendFace() = default;

    // face 정보를 out에 쓴다. 할당 실패는 OutOfMemory다.
    virtual Result<void> Info(FontFaceInfo& out) const noexcept = 0;

    // 크기(96 DPI 포인트)·배율의 줄 metric이다(UE FFreeTypeFace와 같은 계산). 크기·배율이 0 이하면
    // InvalidArgument다.
    virtual Result<FontMetrics> Metrics(float size, float scale) noexcept = 0;

    // code point의 글리프 번호다(없으면 0 = .notdef).
    virtual uint32 GlyphIndex(uint32 codePoint) noexcept = 0;

    // run을 shaping해 글리프를 out 뒤에 붙인다(FullShaping은 HarfBuzz, KerningOnly는 advance +
    // kerning). RTL이면 글리프를 시각 순서로 낸다. 크기·배율이 0 이하면 InvalidArgument, 할당
    // 실패는 OutOfMemory다.
    virtual Result<void> Shape(const ShapeRunRequest& request,
                               Array<ShapedGlyph>& out) noexcept = 0;

    // 글리프 하나를 래스터해 out에 쓴다(UE FSlateFontRenderer::GetRenderData와 같다 — 6.8절
    // FT2d). 크기·배율이 0 이하이거나 글리프 번호가 범위 밖이면 InvalidArgument, FreeType 래스터
    // 실패는 Unsupported(원본 오류를 로그), 할당 실패는 OutOfMemory다.
    virtual Result<void> Rasterize(const GlyphRasterRequest& request,
                                   GlyphBitmap& out) noexcept = 0;
};

class ROCKS_TEXTLAYOUT_API TextBackend {
public:
    virtual ~TextBackend() = default;

    // 글꼴 파일의 하위 face 수다(TTC·OTC면 여럿, 아니면 1). 글꼴이 아니거나 손상이면
    // InvalidArgument다.
    virtual Result<int32> CountSubFaces(const FontFaceData& data) noexcept = 0;

    // face를 연다. data가 없거나 글꼴이 아니거나 하위 face 번호가 범위 밖이면 InvalidArgument,
    // 할당 실패는 OutOfMemory다. face는 desc.data를 붙잡는다.
    virtual Result<UniquePtr<BackendFace>> OpenFace(const FontFaceDesc& desc) noexcept = 0;

    // utf8의 경계 byte 위치를 out에 쓴다(0은 빼고 끝은 넣는다, 오름차순). culture는 경계 규칙
    // 로캘이다 (예 "th", 비면 root). 잘못된 UTF-8은 InvalidArgument다.
    virtual Result<void> FindBoundaries(TextBoundaryKind kind, StringView utf8, StringView culture,
                                        Array<usize>& out) noexcept = 0;

    // utf8의 방향 run을 시각 순서(왼쪽부터)로 out에 쓴다(UAX #9, ICU ubidi_getVisualRun). 빈
    // 문자열은 run이 없다. 잘못된 UTF-8은 InvalidArgument다.
    virtual Result<void> ResolveBidi(StringView utf8, TextBaseDirection base,
                                     Array<BidiRun>& out) noexcept = 0;
};

} // namespace rocks::text
