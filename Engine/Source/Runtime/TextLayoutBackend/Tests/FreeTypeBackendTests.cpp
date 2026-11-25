#include "TextLibrariesTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayoutBackend/TextLayoutBackend.h>
#include <string.h>

using namespace rocks;
namespace freetype_backend_tests_file {

// FreeType backend의 face 열기·정보·metric 시험이다(Docs/Design/Font 6.2·6.8절, FT2a). 기대값은
// 글꼴 표(fontTools로 읽은 Roboto hhea·head·post)에서 손으로 계산했다: Roboto는 unitsPerEm 2048,
// hhea ascender 1900·descender −500·lineGap 0(USE_TYPO_METRICS 꺼짐), bbox yMin −555·yMax 2163,
// post underline −150· 두께 100(FreeType underline_position = −150 − 100/2 = −200). 24pt = 32px이면
// y_scale이 정확히 1.0이라 26.6 값이 글꼴 단위와 같다.

namespace {

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";
constexpr const char* CollectionPath =
    "Engine/Source/ThirdParty/TestFontGenerated/generated-2026-11-24/Roboto-NotoNaskhArabic.ttc";
constexpr const char* EmojiPath =
    "Engine/Source/ThirdParty/TestFontGenerated/generated-2026-11-24/NotoColorEmoji-U1F600.ttf";

// 26.6 고정소수의 1이다.
constexpr float Fixed26Dot6 = 64.0f;

// 시험 크기(24pt, 배율 1 → 32px)다.
constexpr float TestSize = 24.0f;

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
};

} // namespace

TEST(TextLayoutBackend_CreateAndInitializeContracts) {
    CHECK(textlayouttest::EnsureTextLibraries());
    // 할당기 없음은 InvalidArgument다.
    auto none = text::CreateTextBackend({});
    CHECK(!none && none.Error() == ErrorCode::InvalidArgument);
    // 이미 초기화한 뒤 다른 데이터로 다시 부르면 InvalidArgument, 빈 데이터도 InvalidArgument다.
    const uint8 other[] = {0, 1, 2, 3};
    auto mismatch = text::InitializeTextLibraries(Span<const uint8>(other, sizeof(other)));
    CHECK(!mismatch && mismatch.Error() == ErrorCode::InvalidArgument);
    auto empty = text::InitializeTextLibraries(Span<const uint8>());
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);
}

TEST(TextLayoutBackend_CountsSubFacesAndRejectsNonFonts) {
    Fixture fixture;
    CHECK(fixture.Create());
    if (fixture.backend.Get() == nullptr) {
        return;
    }

    auto roboto = fixture.Load(RobotoPath);
    auto collection = fixture.Load(CollectionPath);
    CHECK(roboto.Get() != nullptr && collection.Get() != nullptr);
    if (roboto.Get() == nullptr || collection.Get() == nullptr) {
        return;
    }

    CHECK(fixture.backend->CountSubFaces(*roboto).Value() == 1);
    CHECK(fixture.backend->CountSubFaces(*collection).Value() == 2);
    const uint8 garbage[32] = {'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't'};
    auto notFont = text::FontFaceData::TryCreate(fixture.allocator,
                                                 Span<const uint8>(garbage, sizeof(garbage)));
    CHECK(static_cast<bool>(notFont));
    if (notFont) {
        auto count = fixture.backend->CountSubFaces(*notFont.Value());
        CHECK(!count && count.Error() == ErrorCode::InvalidArgument);
        auto face = fixture.backend->OpenFace({.data = notFont.Value()});
        CHECK(!face && face.Error() == ErrorCode::InvalidArgument);
    }

    // 하위 face 번호: TTC의 1은 Noto Naskh Arabic, 2와 음수는 범위 밖이다. data가 없어도 거부한다.
    auto naskh = fixture.backend->OpenFace({.data = collection, .subFaceIndex = 1});
    CHECK(static_cast<bool>(naskh));
    if (naskh) {
        text::FontFaceInfo info(fixture.allocator);
        CHECK(static_cast<bool>(naskh.Value()->Info(info)));
        CHECK(info.family.View() == StringView("Noto Naskh Arabic"));
    }

    auto outOfRange = fixture.backend->OpenFace({.data = collection, .subFaceIndex = 2});
    CHECK(!outOfRange && outOfRange.Error() == ErrorCode::InvalidArgument);
    auto negative = fixture.backend->OpenFace({.data = collection, .subFaceIndex = -1});
    CHECK(!negative && negative.Error() == ErrorCode::InvalidArgument);
    auto missing = fixture.backend->OpenFace({});
    CHECK(!missing && missing.Error() == ErrorCode::InvalidArgument);
}

TEST(TextLayoutBackend_FaceInfoAndGlyphIndex) {
    Fixture fixture;
    CHECK(fixture.Create());
    auto roboto = fixture.Load(RobotoPath);
    if (fixture.backend.Get() == nullptr || roboto.Get() == nullptr) {
        return;
    }

    auto face = fixture.backend->OpenFace({.data = roboto});
    CHECK(static_cast<bool>(face));
    if (!face) {
        return;
    }

    text::FontFaceInfo info(fixture.allocator);
    CHECK(static_cast<bool>(face.Value()->Info(info)));
    CHECK(info.family.View() == StringView("Roboto"));
    CHECK(info.style.View() == StringView("Regular"));
    CHECK(info.unitsPerEm == 2048u && info.scalable && !info.color && info.glyphCount > 0u);
    CHECK(face.Value()->GlyphIndex('A') != 0u);
    // 없는 code point는 0(.notdef)이다.
    CHECK(face.Value()->GlyphIndex(0x10FFFF) == 0u);
}

TEST(TextLayoutBackend_MetricsMatchUnrealCalculation) {
    Fixture fixture;
    CHECK(fixture.Create());
    auto roboto = fixture.Load(RobotoPath);
    if (fixture.backend.Get() == nullptr || roboto.Get() == nullptr) {
        return;
    }

    auto face = fixture.backend->OpenFace({.data = roboto});
    CHECK(static_cast<bool>(face));
    if (!face) {
        return;
    }

    auto metrics = face.Value()->Metrics(TestSize, 1.0f);
    CHECK(static_cast<bool>(metrics));
    if (metrics) {
        CHECK(metrics.Value().ascender == 1900 / Fixed26Dot6);
        CHECK(metrics.Value().descender == -500 / Fixed26Dot6);
        CHECK(metrics.Value().lineHeight == 2400 / Fixed26Dot6);
        CHECK(metrics.Value().underlinePosition == -200 / Fixed26Dot6);
        CHECK(metrics.Value().underlineThickness == 100 / Fixed26Dot6);
    }

    // 크기·배율이 0 이하면 InvalidArgument다.
    auto zero = face.Value()->Metrics(0.0f, 1.0f);
    CHECK(!zero && zero.Error() == ErrorCode::InvalidArgument);
    auto badScale = face.Value()->Metrics(TestSize, 0.0f);
    CHECK(!badScale && badScale.Error() == ErrorCode::InvalidArgument);

    // BoundingBox: 경계 상자로 줄을 정한다.
    auto bounding = fixture.backend->OpenFace(
        {.data = roboto, .layoutMethod = text::FontLayoutMethod::BoundingBox});
    CHECK(static_cast<bool>(bounding));
    if (bounding) {
        auto boxed = bounding.Value()->Metrics(TestSize, 1.0f);
        CHECK(boxed && boxed.Value().ascender == 2163 / Fixed26Dot6);
        CHECK(boxed && boxed.Value().descender == -555 / Fixed26Dot6);
        CHECK(boxed && boxed.Value().lineHeight == (2163 + 555) / Fixed26Dot6);
    }

    // 재정의: ascent는 26.6 올림(10 → 64), descent는 내림(−10 → −64), 줄 높이 = ascent − descent.
    auto overridden = fixture.backend->OpenFace({.data = roboto,
                                                 .overrideAscent = true,
                                                 .ascentOverride = 10,
                                                 .overrideDescent = true,
                                                 .descentOverride = -10});
    CHECK(static_cast<bool>(overridden));
    if (overridden) {
        auto custom = overridden.Value()->Metrics(TestSize, 1.0f);
        CHECK(custom && custom.Value().ascender == 1.0f);
        CHECK(custom && custom.Value().descender == -1.0f);
        CHECK(custom && custom.Value().lineHeight == 2.0f);
    }
}

TEST(TextLayoutBackend_BitmapFontUsesStrikeScale) {
    // 컬러 비트맵 글꼴(비 scalable): 가장 알맞은 strike를 고르고 그 strike → 요청 크기 배율로
    // metric을 낸다(UE ApplySizeAndScale).
    Fixture fixture;
    CHECK(fixture.Create());
    auto emoji = fixture.Load(EmojiPath);
    if (fixture.backend.Get() == nullptr || emoji.Get() == nullptr) {
        return;
    }

    auto face = fixture.backend->OpenFace({.data = emoji});
    CHECK(static_cast<bool>(face));
    if (!face) {
        return;
    }

    text::FontFaceInfo info(fixture.allocator);
    CHECK(static_cast<bool>(face.Value()->Info(info)));
    CHECK(!info.scalable && info.color && info.fixedSizeCount > 0);
    auto small = face.Value()->Metrics(TestSize, 1.0f);
    auto large = face.Value()->Metrics(TestSize, 2.0f);
    CHECK(small && large);
    if (small && large) {
        CHECK(small.Value().ascender > 0.0f && small.Value().lineHeight > 0.0f);
        // 배율이 두 배면 줄 높이도 두 배다(같은 strike, 배율만 바뀐다).
        const float ratio = large.Value().lineHeight / small.Value().lineHeight;
        CHECK(ratio > 1.99f && ratio < 2.01f);
    }
}

} // namespace freetype_backend_tests_file
