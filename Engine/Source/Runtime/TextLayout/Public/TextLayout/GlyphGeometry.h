#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

// 글리프 전처리 geometry와 거리장 요청 형식이다(Docs/Design/Font 6.5·6.8절 FT2e — UE
// FPreprocessedFontGeometry·FSlateSdfGenerator). geometry는 face 없이 거리장을 만들 수 있게 윤곽을
// 정수로 담는다. 형식과 버전은 TextLayout이 소유한다.
//
// 사용 예:
//   text::GlyphGeometry geometry(allocator);
//   face->ExtractGeometry(glyph, geometry);
//   Array<uint8> bytes(allocator);
//   text::WriteGlyphGeometry(geometry, bytes);
namespace rocks::text {

// 글자 래스터 방식이다(UE EFontRasterizationMode — 6.5절 표).
enum class FontRasterMode : uint8 {
    Bitmap,
    Msdf,
    Sdf,
    SdfApproximation,
};

// edge 종류다. 값 + 1이 제어점 수다(msdfgen EDGE_TYPE과 같다).
enum class GeometryEdgeKind : uint8 {
    Line = 1,
    Quadratic = 2,
    Cubic = 3,
};

// 직렬화 형식 버전이다. 바꾸면 쿠킹 산출물을 다시 만든다.
inline constexpr uint32 GlyphGeometryFormatVersion = 1;

// 좌표는 글꼴 단위 정수다(FreeType FT_Outline_Decompose가 생략된 on-curve 중점도 정수로 계산한다).
struct GeometryPoint {
    int32 x = 0;
    int32 y = 0;

    friend constexpr bool operator==(const GeometryPoint&, const GeometryPoint&) noexcept = default;
};

// 글리프 하나의 윤곽이다. edge마다 제어점(종류 + 1개)을 points에 차례로 둔다.
struct GlyphGeometry {
    explicit GlyphGeometry(Allocator& allocator) noexcept
        : contourEdgeEnds(allocator), edgeKinds(allocator), points(allocator) {
    }

    void Clear() noexcept {
        unitsPerEm = 0;
        contourEdgeEnds.Clear();
        edgeKinds.Clear();
        points.Clear();
    }

    bool Empty() const noexcept {
        return edgeKinds.Size() == 0;
    }

    uint16 unitsPerEm = 0;
    // 윤곽마다 마지막 edge 다음 번호다(오름차순).
    Array<uint32> contourEdgeEnds;
    Array<GeometryEdgeKind> edgeKinds;
    Array<GeometryPoint> points;
};

// geometry를 out 뒤에 쓴다(버전·unitsPerEm·윤곽·edge·점, little-endian). 할당 실패는
// OutOfMemory다.
ROCKS_TEXTLAYOUT_API Result<void> WriteGlyphGeometry(const GlyphGeometry& geometry,
                                                     Array<uint8>& out) noexcept;

// bytes를 읽어 out에 쓴다. 버전이 다르면 Unsupported(다시 만든다), 잘렸거나 모순(edge 종류·점 수·
// 윤곽 끝)이면 InvalidArgument다.
ROCKS_TEXTLAYOUT_API Result<void> ReadGlyphGeometry(Span<const uint8> bytes,
                                                    GlyphGeometry& out) noexcept;

// 거리장 생성 요청이다(UE FSlateSdfGenerator::FRequestDescriptor).
struct DistanceFieldRequest {
    // 참이면 MTSDF 4채널(Msdf), 거짓이면 단일 채널(Sdf)이다.
    bool multichannel = true;
    // 참이면 빠른 근사(자리표시·SdfApproximation — UE MakePlaceholder)다.
    bool approximate = false;
    // 거리장 해상도(em당 화소)다.
    uint32 ppem = 0;
    // em 단위 바깥·안쪽 spread다.
    float emOuterSpread = 0.0f;
    float emInnerSpread = 0.0f;
};

} // namespace rocks::text
