#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <TextLayout/TextBackend.h>

// 실행용 조합 글꼴과 글자별 face 탐색이다(Docs/Design/Font 6.3·6.8절, UE FCompositeFontCache). 저장
// 기술 (Font 모듈 CompositeFontDesc)은 face 참조를 FontFaceDesc로 풀어 CompositeFontSpec을 만든다.
//
// 사용 예:
//   const text::CompositeTypefaceEntry regular[] = {{.name = "Regular", .face = robotoDesc}};
//   const text::CompositeFontSpec spec{.defaultTypeface = regular};
//   auto font = text::ResolvedCompositeFont::TryCreate(allocator, backend, spec);
//   auto selection = font.Value()->Select('A', "Regular", {}, text::FontFallback::None);
namespace rocks::text {

// 보조 typeface의 문자 구간이다(닫힌 구간, first ≤ last ≤ 0x10FFFF).
struct CodePointRange {
    uint32 first = 0;
    uint32 last = 0;
};

// typeface의 이름 있는 face 하나다(UE FTypefaceEntry).
struct CompositeTypefaceEntry {
    StringView name = {};
    FontFaceDesc face = {};
};

// 보조 typeface다(UE FCompositeSubFont). cultures가 비지 않으면 그 culture(또는 부모)일 때만 쓴다.
struct CompositeSubTypeface {
    Span<const CompositeTypefaceEntry> typeface = {};
    float scalingFactor = 1.0f;
    Span<const CodePointRange> ranges = {};
    Span<const StringView> cultures = {};
};

// 실행용 조합 기술이다(UE FCompositeFont). fallback typeface와 마지막 fallback face는 없어도 된다.
struct CompositeFontSpec {
    Span<const CompositeTypefaceEntry> defaultTypeface = {};
    Span<const CompositeTypefaceEntry> fallbackTypeface = {};
    float fallbackScalingFactor = 1.0f;
    Span<const CompositeSubTypeface> subTypefaces = {};
    // 마지막 fallback 글꼴(UE LastResort.ttf — 쿠킹 안 된 실행 전용)이다. data가 없으면 쓰지
    // 않는다.
    FontFaceDesc lastResort = {};
};

// 마지막 fallback까지 가는지다(UE EFontFallback — 4.1절).
enum class FontFallback : uint8 {
    None,
    LastResort,
    All,
};

// typeface 이름이 맞는 entry가 없을 때 고르는 속성이다(UE GetBestMatchFontForAttributes).
struct FontStyleRequest {
    bool bold = false;
    bool italic = false;
};

// 글자 하나에 고른 face다. glyphIndex가 0이면 어느 face에도 글리프가 없어 .notdef를 그린다.
struct FontSelection {
    BackendFace* face = nullptr;
    float scalingFactor = 1.0f;
    uint32 glyphIndex = 0;
    // 실제로 고른 code point다(글리프가 없어 U+FFFD로 다시 골랐으면 0xFFFD).
    uint32 codePoint = 0;
};

// ResolvedCompositeFont의 내부 표다(사용자가 직접 만들지 않는다).
struct CompositeFontEntry {
    explicit CompositeFontEntry(Allocator& allocator) noexcept : name(allocator) {
    }

    String name;
    UniquePtr<BackendFace> face;
    bool bold = false;
    bool italic = false;
};

struct CompositeFontTypeface {
    explicit CompositeFontTypeface(Allocator& allocator) noexcept : entries(allocator) {
    }

    Array<CompositeFontEntry> entries;
    float scalingFactor = 1.0f;
};

struct CompositeFontSubTypeface {
    explicit CompositeFontSubTypeface(Allocator& allocator) noexcept
        : typeface(allocator), ranges(allocator), cultures(allocator) {
    }

    CompositeFontTypeface typeface;
    Array<CodePointRange> ranges;
    Array<String> cultures;
};

// 구간 하나와 그 보조 typeface 번호·선언 순서다(겹치면 작은 구간, 같으면 선언 순서가 이긴다).
struct CompositeFontRange {
    uint32 first = 0;
    uint32 last = 0;
    int32 subTypeface = 0;
    uint32 order = 0;
};

class ROCKS_TEXTLAYOUT_API ResolvedCompositeFont {
public:
    // spec의 모든 face를 연다. 기본 typeface가 비었거나 배율이 0 이하이거나 구간이 잘못되었으면
    // InvalidArgument, face 열기 실패는 그 오류다. spec의 Span은 이 호출 동안만
    // 읽는다(이름·culture는 복사한다). backend는 이 객체보다 오래 살아야 한다.
    static Result<UniquePtr<ResolvedCompositeFont>>
    TryCreate(Allocator& allocator, TextBackend& backend, const CompositeFontSpec& spec) noexcept;

    explicit ResolvedCompositeFont(Allocator& allocator) noexcept;

    // culture(예: "ko-KR")를 정하고 culture 우선 구간 표를 다시 만든다. 빈 culture는 우선 구간이
    // 없다. 할당 실패는 OutOfMemory다(이전 culture가 남는다).
    Result<void> SetCulture(StringView culture) noexcept;

    // 지금 culture다(SetCulture의 값, 처음은 빈 문자열).
    StringView Culture() const noexcept {
        return culture_.View();
    }

    // code point에 쓸 face를 고른다(6.3절 규칙). 항상 face 하나를 준다(기본 typeface가 있으므로).
    FontSelection Select(uint32 codePoint, StringView typefaceName, FontStyleRequest style,
                         FontFallback fallback) noexcept;

private:
    // typeface index의 face 하나를 고른다(이름 → 속성 → 첫 entry).
    CompositeFontEntry& PickEntry(CompositeFontTypeface& typeface, StringView typefaceName,
                                  FontStyleRequest style) noexcept;
    // 구간 표에서 code point를 덮는 가장 작은 구간의 보조 typeface 번호다(없으면 −1).
    int32 FindRange(const Array<CompositeFontRange>& ranges, uint32 codePoint) const noexcept;
    FontSelection SelectOnce(uint32 codePoint, StringView typefaceName, FontStyleRequest style,
                             FontFallback fallback) noexcept;

    Allocator& allocator_;
    CompositeFontTypeface defaultTypeface_;
    CompositeFontTypeface fallbackTypeface_;
    Array<CompositeFontSubTypeface> subTypefaces_;
    Array<CompositeFontRange> priorityRanges_;
    Array<CompositeFontRange> generalRanges_;
    UniquePtr<BackendFace> lastResort_;
    String culture_;
};

} // namespace rocks::text
