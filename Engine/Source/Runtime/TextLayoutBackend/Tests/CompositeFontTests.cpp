#include "TextLibrariesTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/CompositeFont.h>
#include <TextLayoutBackend/TextLayoutBackend.h>

using namespace rocks;
namespace composite_font_tests_file {

// 조합 글꼴 탐색 시험이다(Docs/Design/Font 6.3·6.8절, FT2b). 시험 글꼴의 문자 보유(fontTools로
// 확인): Roboto는 'A'·U+FFFD가 있고 한글·아랍어·이모지가 없다. NanumGothic은 'A'·'한'이 있고
// U+FFFD가 없다. Noto Naskh Arabic은 'A'·'س'가 있다. 컬러 이모지 부분집합은 U+1F600만 있다.

namespace {

constexpr uint32 LetterA = 0x41;
constexpr uint32 HangulHan = 0xD55C;
constexpr uint32 ArabicSeen = 0x633;
constexpr uint32 GrinningFace = 0x1F600;
constexpr uint32 Replacement = 0xFFFD;

// 한글 음절 구간(U+AC00–U+D7A3)과 BMP 전체 구간이다.
constexpr text::CodePointRange HangulSyllables{0xAC00, 0xD7A3};
constexpr text::CodePointRange BasicPlane{0x0000, 0xFFFF};

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";
constexpr const char* NanumPath =
    "Engine/Source/ThirdParty/TestFontNanumGothic/google-fonts-06bfe0d62a98/ofl/nanumgothic/"
    "NanumGothic-Regular.ttf";
constexpr const char* NaskhPath =
    "Engine/Source/ThirdParty/TestFontNotoNaskhArabic/google-fonts-06bfe0d62a98/ofl/"
    "notonaskharabic/NotoNaskhArabic-wght.ttf";
constexpr const char* EmojiPath =
    "Engine/Source/ThirdParty/TestFontGenerated/generated-2026-11-24/NotoColorEmoji-U1F600.ttf";

struct Fixture {
    SystemAllocator allocator;
    UniquePtr<text::TextBackend> backend;
    SharedPtr<const text::FontFaceData> roboto;
    SharedPtr<const text::FontFaceData> nanum;
    SharedPtr<const text::FontFaceData> naskh;
    SharedPtr<const text::FontFaceData> emoji;

    bool Create() noexcept {
        if (!textlayouttest::EnsureTextLibraries()) {
            return false;
        }

        auto created = text::CreateTextBackend({.allocator = &allocator});
        if (!created) {
            return false;
        }

        backend = Move(created.Value());
        roboto = Load(RobotoPath);
        nanum = Load(NanumPath);
        naskh = Load(NaskhPath);
        emoji = Load(EmojiPath);
        return roboto.Get() != nullptr && nanum.Get() != nullptr && naskh.Get() != nullptr &&
               emoji.Get() != nullptr;
    }

    SharedPtr<const text::FontFaceData> Load(const char* path) noexcept {
        Array<uint8> bytes(allocator);
        if (!textlayouttest::ReadTestFile(path, bytes)) {
            return SharedPtr<const text::FontFaceData>();
        }

        auto data =
            text::FontFaceData::TryCreate(allocator, Span<const uint8>(bytes.Data(), bytes.Size()));
        return data ? Move(data.Value()) : SharedPtr<const text::FontFaceData>();
    }

    UniquePtr<text::ResolvedCompositeFont> Resolve(const text::CompositeFontSpec& spec) noexcept {
        auto font = text::ResolvedCompositeFont::TryCreate(allocator, *backend, spec);
        return font ? Move(font.Value()) : UniquePtr<text::ResolvedCompositeFont>();
    }
};

// face의 family 이름이 name이면 true다.
bool IsFamily(Allocator& allocator, text::BackendFace* face, StringView name) noexcept {
    if (face == nullptr) {
        return false;
    }

    text::FontFaceInfo info(allocator);
    return static_cast<bool>(face->Info(info)) && info.family.View() == name;
}

} // namespace

TEST(TextLayout_CompositeDefaultTypefaceAndReplacement) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    const text::CompositeTypefaceEntry regular[] = {
        {.name = "Regular", .face = {.data = fixture.roboto}}};
    auto font = fixture.Resolve({.defaultTypeface = regular});
    CHECK(font.Get() != nullptr);
    if (font.Get() == nullptr) {
        return;
    }

    const text::FontSelection latin =
        font->Select(LetterA, "Regular", {}, text::FontFallback::None);
    CHECK(IsFamily(fixture.allocator, latin.face, "Roboto") && latin.glyphIndex != 0 &&
          latin.codePoint == LetterA);
    // 어디에도 없는 글자는 U+FFFD로 다시 고른다(Roboto에 U+FFFD가 있다).
    const text::FontSelection missing =
        font->Select(HangulHan, "Regular", {}, text::FontFallback::None);
    CHECK(missing.codePoint == Replacement && missing.glyphIndex != 0);
}

TEST(TextLayout_CompositeSubTypefaceRangesSmallerWins) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    const text::CompositeTypefaceEntry latin[] = {
        {.name = "Regular", .face = {.data = fixture.roboto}}};
    const text::CompositeTypefaceEntry korean[] = {
        {.name = "Regular", .face = {.data = fixture.nanum}}};
    const text::CompositeTypefaceEntry arabic[] = {
        {.name = "Regular", .face = {.data = fixture.naskh}}};
    const text::CodePointRange plane[] = {BasicPlane};
    const text::CodePointRange hangul[] = {HangulSyllables};
    // 넓은 구간(Naskh, BMP 전체)을 먼저 선언해도 작은 구간(Nanum, 한글)이 이긴다.
    const text::CompositeSubTypeface subs[] = {
        {.typeface = arabic, .ranges = plane},
        {.typeface = korean, .scalingFactor = 1.25f, .ranges = hangul}};
    auto font = fixture.Resolve({.defaultTypeface = latin, .subTypefaces = subs});
    CHECK(font.Get() != nullptr);
    if (font.Get() == nullptr) {
        return;
    }

    const text::FontSelection han = font->Select(HangulHan, {}, {}, text::FontFallback::None);
    CHECK(IsFamily(fixture.allocator, han.face, "NanumGothic") && han.scalingFactor == 1.25f &&
          han.glyphIndex != 0);
    const text::FontSelection seen = font->Select(ArabicSeen, {}, {}, text::FontFallback::None);
    CHECK(IsFamily(fixture.allocator, seen.face, "Noto Naskh Arabic") && seen.glyphIndex != 0);
    const text::FontSelection a = font->Select(LetterA, {}, {}, text::FontFallback::None);
    CHECK(IsFamily(fixture.allocator, a.face, "Noto Naskh Arabic"));
    // 고른 보조 typeface에 글리프가 없으면 기본 typeface로 간다(이모지는 BMP 밖이라 구간 밖 → 기본
    // → U+FFFD).
    const text::FontSelection face = font->Select(GrinningFace, {}, {}, text::FontFallback::None);
    CHECK(IsFamily(fixture.allocator, face.face, "Roboto") && face.codePoint == Replacement);
}

TEST(TextLayout_CompositeCulturePriority) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    const text::CompositeTypefaceEntry latin[] = {
        {.name = "Regular", .face = {.data = fixture.roboto}}};
    const text::CompositeTypefaceEntry korean[] = {
        {.name = "Regular", .face = {.data = fixture.nanum}}};
    const text::CodePointRange hangul[] = {HangulSyllables};
    const StringView koreanCultures[] = {"ko"};
    const text::CompositeSubTypeface subs[] = {
        {.typeface = korean, .ranges = hangul, .cultures = koreanCultures}};
    auto font = fixture.Resolve({.defaultTypeface = latin, .subTypefaces = subs});
    CHECK(font.Get() != nullptr);
    if (font.Get() == nullptr) {
        return;
    }

    // culture 지정 보조 typeface는 그 culture가 아니면 무시한다.
    CHECK(IsFamily(fixture.allocator,
                   font->Select(HangulHan, {}, {}, text::FontFallback::None).face, "Roboto"));
    CHECK(static_cast<bool>(font->SetCulture("ko-KR")));
    CHECK(IsFamily(fixture.allocator,
                   font->Select(HangulHan, {}, {}, text::FontFallback::None).face, "NanumGothic"));
    CHECK(static_cast<bool>(font->SetCulture("ko")));
    CHECK(IsFamily(fixture.allocator,
                   font->Select(HangulHan, {}, {}, text::FontFallback::None).face, "NanumGothic"));
    // "kor"는 "ko"의 자식이 아니다(구분자 뒤만 부모로 본다).
    CHECK(static_cast<bool>(font->SetCulture("kor")));
    CHECK(IsFamily(fixture.allocator,
                   font->Select(HangulHan, {}, {}, text::FontFallback::None).face, "Roboto"));
    CHECK(static_cast<bool>(font->SetCulture("ja")));
    CHECK(IsFamily(fixture.allocator,
                   font->Select(HangulHan, {}, {}, text::FontFallback::None).face, "Roboto"));
}

TEST(TextLayout_CompositeFallbackTypefaceAndLastResort) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    const text::CompositeTypefaceEntry latin[] = {
        {.name = "Regular", .face = {.data = fixture.roboto}}};
    const text::CompositeTypefaceEntry korean[] = {
        {.name = "Regular", .face = {.data = fixture.nanum}}};
    // fallback typeface: 기본에 없는 글자를 받는다(배율 적용).
    auto withFallback = fixture.Resolve(
        {.defaultTypeface = latin, .fallbackTypeface = korean, .fallbackScalingFactor = 0.9f});
    CHECK(withFallback.Get() != nullptr);
    if (withFallback.Get() != nullptr) {
        const text::FontSelection han =
            withFallback->Select(HangulHan, {}, {}, text::FontFallback::None);
        CHECK(IsFamily(fixture.allocator, han.face, "NanumGothic") && han.scalingFactor == 0.9f);
    }

    // 마지막 fallback: FontFallback이 LastResort 이상일 때만 쓴다.
    auto withLastResort =
        fixture.Resolve({.defaultTypeface = latin, .lastResort = {.data = fixture.emoji}});
    CHECK(withLastResort.Get() != nullptr);
    if (withLastResort.Get() != nullptr) {
        const text::FontSelection none =
            withLastResort->Select(GrinningFace, {}, {}, text::FontFallback::None);
        CHECK(none.codePoint == Replacement);
        const text::FontSelection last =
            withLastResort->Select(GrinningFace, {}, {}, text::FontFallback::LastResort);
        CHECK(last.codePoint == GrinningFace && last.glyphIndex != 0);
        CHECK(withLastResort->Select(GrinningFace, {}, {}, text::FontFallback::All).glyphIndex !=
              0);
    }
}

TEST(TextLayout_CompositeTypefaceNameAndInvalidSpecs) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    const text::CompositeTypefaceEntry entries[] = {
        {.name = "Latin", .face = {.data = fixture.roboto}},
        {.name = "Korean", .face = {.data = fixture.nanum}}};
    auto font = fixture.Resolve({.defaultTypeface = entries});
    CHECK(font.Get() != nullptr);
    if (font.Get() != nullptr) {
        CHECK(IsFamily(fixture.allocator,
                       font->Select(LetterA, "Korean", {}, text::FontFallback::None).face,
                       "NanumGothic"));
        // 없는 이름은 속성 일치(둘 다 Regular라 같다) → 먼저 선언한 entry다.
        CHECK(IsFamily(fixture.allocator,
                       font->Select(LetterA, "Missing", {}, text::FontFallback::None).face,
                       "Roboto"));
    }

    // 빈 기본 typeface, 역전·범위 밖 구간, 0 배율은 InvalidArgument다.
    auto empty = text::ResolvedCompositeFont::TryCreate(fixture.allocator, *fixture.backend, {});
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);
    const text::CodePointRange reversed[] = {{0x200, 0x100}};
    const text::CompositeSubTypeface badRange[] = {{.typeface = entries, .ranges = reversed}};
    auto badRangeFont = text::ResolvedCompositeFont::TryCreate(
        fixture.allocator, *fixture.backend,
        {.defaultTypeface = entries, .subTypefaces = badRange});
    CHECK(!badRangeFont && badRangeFont.Error() == ErrorCode::InvalidArgument);
    const text::CodePointRange beyond[] = {{0x10FFF0, 0x110000}};
    const text::CompositeSubTypeface beyondRange[] = {{.typeface = entries, .ranges = beyond}};
    auto beyondFont = text::ResolvedCompositeFont::TryCreate(
        fixture.allocator, *fixture.backend,
        {.defaultTypeface = entries, .subTypefaces = beyondRange});
    CHECK(!beyondFont && beyondFont.Error() == ErrorCode::InvalidArgument);
    const text::CompositeSubTypeface zeroScale[] = {{.typeface = entries, .scalingFactor = 0.0f}};
    auto zeroFont = text::ResolvedCompositeFont::TryCreate(
        fixture.allocator, *fixture.backend,
        {.defaultTypeface = entries, .subTypefaces = zeroScale});
    CHECK(!zeroFont && zeroFont.Error() == ErrorCode::InvalidArgument);
}

} // namespace composite_font_tests_file
