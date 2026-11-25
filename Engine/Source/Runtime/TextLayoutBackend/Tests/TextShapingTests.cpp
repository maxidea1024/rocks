#include "TextLibrariesTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/TextShaper.h>
#include <TextLayoutBackend/TextLayoutBackend.h>

using namespace rocks;
namespace text_shaping_tests_file {

// shaping·경계·BiDi 시험이다(Docs/Design/Font 6.4·6.8절, FT2c). FT1의 ICU·HarfBuzz 직접 연결 시험이
// 보던 것(태국어 사전 경계, ZWJ grapheme, BiDi run, 합자, 아랍어 RTL 문맥형)을 backend API로 다시
// 본다.

namespace {

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";
constexpr const char* NanumPath =
    "Engine/Source/ThirdParty/TestFontNanumGothic/google-fonts-06bfe0d62a98/ofl/nanumgothic/"
    "NanumGothic-Regular.ttf";
constexpr const char* NaskhPath =
    "Engine/Source/ThirdParty/TestFontNotoNaskhArabic/google-fonts-06bfe0d62a98/ofl/"
    "notonaskharabic/NotoNaskhArabic-wght.ttf";

constexpr text::CodePointRange HangulSyllables{0xAC00, 0xD7A3};

// "안녕하세요 세계", "สวัสดีครับ", 가족 이모지 ZWJ 열(U+1F468 ZWJ U+1F469 ZWJ U+1F467), "abc אבג",
// "سلام"이다.
constexpr const char* Korean =
    "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94 \xEC\x84\xB8\xEA\xB3\x84";
constexpr const char* Thai =
    "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5"
    "\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A";
constexpr const char* Family =
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
constexpr const char* MixedBidi = "abc \xD7\x90\xD7\x91\xD7\x92";
constexpr const char* RtlFirst = "\xD7\x90\xD7\x91\xD7\x92 abc";
constexpr const char* Salam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";

struct Fixture {
    SystemAllocator allocator;
    UniquePtr<text::TextBackend> backend;

    bool Create() noexcept {
        if (!textlayouttest::EnsureTextLibraries()) {
            return false;
        }

        auto created = text::CreateTextBackend({.allocator = &allocator});
        if (!created) {
            return false;
        }

        backend = Move(created.Value());
        return true;
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

    UniquePtr<text::ResolvedCompositeFont> Single(const char* path) noexcept {
        const text::CompositeTypefaceEntry entry[] = {
            {.name = "Regular", .face = {.data = Load(path)}}};
        auto font =
            text::ResolvedCompositeFont::TryCreate(allocator, *backend, {.defaultTypeface = entry});
        return font ? Move(font.Value()) : UniquePtr<text::ResolvedCompositeFont>();
    }
};

bool Boundaries(Fixture& fixture, text::TextBoundaryKind kind, StringView text, StringView culture,
                Span<const usize> expected) noexcept {
    Array<usize> found(fixture.allocator);
    if (!fixture.backend->FindBoundaries(kind, text, culture, found) ||
        found.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (found[index] != expected[index]) {
            return false;
        }
    }

    return true;
}

} // namespace

TEST(TextLayoutBackend_BoundariesUseIcuData) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    // 줄: "hello world"는 공백 뒤(6)와 끝(11)에서 나눌 수 있다.
    const usize line[] = {6, 11};
    CHECK(Boundaries(fixture, text::TextBoundaryKind::Line, "hello world", "en", line));
    // 단어: 한국어 두 어절(15 byte·공백·6 byte)은 어절 끝·공백 끝·문자열 끝이다.
    const usize words[] = {15, 16, 22};
    CHECK(Boundaries(fixture, text::TextBoundaryKind::Word, Korean, "ko", words));
    // 단어: 태국어는 공백이 없어 사전(thaidict)이 สวัสดี(18 byte)와 ครับ(12 byte)으로 나눈다.
    const usize thai[] = {18, 30};
    CHECK(Boundaries(fixture, text::TextBoundaryKind::Word, Thai, "th", thai));
    // grapheme: ZWJ 이모지 열(18 byte)은 하나다.
    const usize family[] = {18};
    CHECK(Boundaries(fixture, text::TextBoundaryKind::Grapheme, Family, {}, family));
    // 잘못된 UTF-8은 InvalidArgument다.
    Array<usize> found(fixture.allocator);
    auto bad = fixture.backend->FindBoundaries(text::TextBoundaryKind::Grapheme,
                                               StringView("\xFF", 1), {}, found);
    CHECK(!bad && bad.Error() == ErrorCode::InvalidArgument);
}

TEST(TextLayoutBackend_BidiRunsInVisualOrder) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    Array<text::BidiRun> runs(fixture.allocator);
    CHECK(static_cast<bool>(
        fixture.backend->ResolveBidi(MixedBidi, text::TextBaseDirection::Auto, runs)));
    CHECK(runs.Size() == 2u);
    if (runs.Size() == 2u) {
        CHECK(!runs[0].rightToLeft && runs[0].start == 0u && runs[0].length == 4u);
        CHECK(runs[1].rightToLeft && runs[1].start == 4u && runs[1].length == 6u);
    }

    // RTL 문단 안의 LTR 구간은 시각적으로 왼쪽(먼저)에 온다.
    CHECK(static_cast<bool>(
        fixture.backend->ResolveBidi(RtlFirst, text::TextBaseDirection::RightToLeft, runs)));
    CHECK(runs.Size() == 2u);
    if (runs.Size() == 2u) {
        CHECK(!runs[0].rightToLeft && runs[0].start > runs[1].start && runs[1].rightToLeft);
    }

    CHECK(static_cast<bool>(fixture.backend->ResolveBidi({}, text::TextBaseDirection::Auto, runs)));
    CHECK(runs.Size() == 0u);
}

TEST(TextLayoutBackend_ShapeLigaturesAndMethods) {
    Fixture fixture;
    CHECK(fixture.Create());
    auto font = fixture.backend.Get() != nullptr ? fixture.Single(RobotoPath)
                                                 : UniquePtr<text::ResolvedCompositeFont>();
    CHECK(font.Get() != nullptr);
    if (font.Get() == nullptr) {
        return;
    }

    Array<text::ShapedGlyph> glyphs(fixture.allocator);
    // Auto: 라틴 LTR은 KerningOnly라 합자가 없다(UE와 같다).
    CHECK(static_cast<bool>(text::ShapeText(*fixture.backend, *font, "fin", {}, glyphs)));
    CHECK(glyphs.Size() == 3u);
    // FullShaping: "fi" 합자 → 글리프 2개, cluster는 0과 2다.
    CHECK(static_cast<bool>(text::ShapeText(
        *fixture.backend, *font, "fin", {.method = text::TextShapingMethod::FullShaping}, glyphs)));
    CHECK(glyphs.Size() == 2u && glyphs[0].cluster == 0u && glyphs[1].cluster == 2u);
    // 자간이 있으면 합자를 끄고 advance에 자간을 더한다(24pt·자간 100 → 2.4).
    Array<text::ShapedGlyph> spaced(fixture.allocator);
    CHECK(static_cast<bool>(text::ShapeText(
        *fixture.backend, *font, "fin",
        {.method = text::TextShapingMethod::FullShaping, .letterSpacing = 100}, spaced)));
    CHECK(spaced.Size() == 3u);
    // KerningOnly: "AV"는 kerning(GPOS 쌍 — FD19)으로 A·V를 따로 잰 합보다 좁다.
    Array<text::ShapedGlyph> pair(fixture.allocator);
    Array<text::ShapedGlyph> a(fixture.allocator);
    Array<text::ShapedGlyph> v(fixture.allocator);
    CHECK(static_cast<bool>(text::ShapeText(*fixture.backend, *font, "AV", {}, pair)));
    CHECK(static_cast<bool>(text::ShapeText(*fixture.backend, *font, "A", {}, a)));
    CHECK(static_cast<bool>(text::ShapeText(*fixture.backend, *font, "V", {}, v)));
    CHECK(text::MeasureWidth(pair.View()) <
          text::MeasureWidth(a.View()) + text::MeasureWidth(v.View()));
    // 고정폭: advance = monospacedWidth × size × scale.
    CHECK(static_cast<bool>(
        text::ShapeText(*fixture.backend, *font, "il", {.monospaced = true}, glyphs)));
    CHECK(glyphs.Size() == 2u && glyphs[0].advance == 24.0f && glyphs[1].advance == 24.0f);
}

TEST(TextLayoutBackend_ShapeArabicAndMixedFaces) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    auto arabic = fixture.Single(NaskhPath);
    CHECK(arabic.Get() != nullptr);
    if (arabic.Get() != nullptr) {
        Array<text::ShapedGlyph> glyphs(fixture.allocator);
        CHECK(static_cast<bool>(text::ShapeText(*fixture.backend, *arabic, Salam, {}, glyphs)));
        // RTL은 시각 순서(왼쪽부터)라 첫 글리프가 마지막 글자다.
        CHECK(glyphs.Size() >= 3u && glyphs[0].cluster > glyphs[glyphs.Size() - 1].cluster);
        // 문맥형: 낱말 첫머리 'س'는 단독 'س'와 다른 글리프다.
        Array<text::ShapedGlyph> alone(fixture.allocator);
        CHECK(static_cast<bool>(
            text::ShapeText(*fixture.backend, *arabic, StringView(Salam, 2), {}, alone)));
        CHECK(alone.Size() == 1u && glyphs.Size() != 0 &&
              alone[0].glyphIndex != glyphs[glyphs.Size() - 1].glyphIndex);
    }

    // 조합: 라틴은 Roboto, 한글은 Nanum(보조 typeface)이다.
    const text::CompositeTypefaceEntry latin[] = {
        {.name = "Regular", .face = {.data = fixture.Load(RobotoPath)}}};
    const text::CompositeTypefaceEntry korean[] = {
        {.name = "Regular", .face = {.data = fixture.Load(NanumPath)}}};
    const text::CodePointRange hangul[] = {HangulSyllables};
    const text::CompositeSubTypeface subs[] = {{.typeface = korean, .ranges = hangul}};
    auto mixed = text::ResolvedCompositeFont::TryCreate(
        fixture.allocator, *fixture.backend, {.defaultTypeface = latin, .subTypefaces = subs});
    CHECK(static_cast<bool>(mixed));
    if (mixed) {
        Array<text::ShapedGlyph> glyphs(fixture.allocator);
        CHECK(static_cast<bool>(
            text::ShapeText(*fixture.backend, *mixed.Value(), "A\xED\x95\x9C", {}, glyphs)));
        CHECK(glyphs.Size() == 2u && glyphs[0].face != glyphs[1].face && glyphs[1].cluster == 1u);
        CHECK(glyphs.Size() == 2u && glyphs[0].advance > 0.0f && glyphs[1].advance > 0.0f);
    }
}

TEST(TextLayoutBackend_ShapeRejectsBadInput) {
    Fixture fixture;
    CHECK(fixture.Create());
    auto font = fixture.backend.Get() != nullptr ? fixture.Single(RobotoPath)
                                                 : UniquePtr<text::ResolvedCompositeFont>();
    if (font.Get() == nullptr) {
        return;
    }

    Array<text::ShapedGlyph> glyphs(fixture.allocator);
    auto badText = text::ShapeText(*fixture.backend, *font, StringView("\xC3", 1), {}, glyphs);
    CHECK(!badText && badText.Error() == ErrorCode::InvalidArgument);
    auto badSize = text::ShapeText(*fixture.backend, *font, "a", {.size = 0.0f}, glyphs);
    CHECK(!badSize && badSize.Error() == ErrorCode::InvalidArgument);
    CHECK(static_cast<bool>(text::ShapeText(*fixture.backend, *font, {}, {}, glyphs)));
    CHECK(glyphs.Size() == 0u);
}

} // namespace text_shaping_tests_file
