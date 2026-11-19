#include <Base/NumericLimits.h>
#include <Core/Memory/SystemAllocator.h>
#include <ReflectionText/TextDiagnostics.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 리플렉션 B9i3a 진단 모음 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.2절 "진단"): 위치
// 종류(대입 순번·JSON 위치)와 텍스트 복사, 한도 안에서 모두 모으고 넘친 수 세기, 할당 실패의
// 무변경, 정의되지 않은 값 거부, 비우기.
using namespace rocks;
namespace text_diagnostics_tests_file {

using namespace rocks::reflection_text;

namespace {

bool Same(StringView left, StringView right) noexcept {
    return left == right;
}

} // namespace

// 진단은 Add 순서로 저장되고 path·message는 복사본이다(호출자 버퍼를 바꿔도 남는다). 위치는 대입
// 순번·JSON 위치·없음 중 하나이고, 진단 이름은 열거자 철자다.
TEST(TextDiagnostics_CollectsInOrderWithSources) {
    SystemAllocator system;
    TextDiagnostics diagnostics(system);
    CHECK(!diagnostics.HasErrors());
    CHECK(diagnostics.Limit() == DefaultMaxTextDiagnostics);

    char path[] = "SourceColorSettings.ColorSpace";
    REQUIRE(diagnostics.Add(TextDiagnosticCode::UnknownProperty, TextSource::Assignment(3), path,
                            "unknown property"));
    path[0] = 'X';
    JsonPosition position;
    position.line = 4;
    position.column = 9;
    position.byteOffset = 57;
    REQUIRE(diagnostics.Add(TextDiagnosticCode::DuplicateKey, TextSource::Json(position),
                            "perPlatform", "keys differ only by case"));
    REQUIRE(diagnostics.Add(TextDiagnosticCode::Unsupported, TextSource{}, {}, "no resolver"));

    REQUIRE(diagnostics.Count() == 3);
    CHECK(diagnostics.HasErrors());
    CHECK(diagnostics.Omitted() == 0);

    const TextDiagnostic first = diagnostics.Get(0);
    CHECK(first.code == TextDiagnosticCode::UnknownProperty);
    CHECK(first.source.kind == TextSourceKind::Assignment);
    CHECK(first.source.sourceIndex == 3);
    CHECK(Same(first.path, "SourceColorSettings.ColorSpace"));
    CHECK(Same(first.message, "unknown property"));

    const TextDiagnostic second = diagnostics.Get(1);
    CHECK(second.code == TextDiagnosticCode::DuplicateKey);
    CHECK(second.source.kind == TextSourceKind::Json);
    CHECK(second.source.position.line == 4);
    CHECK(second.source.position.column == 9);
    CHECK(second.source.position.byteOffset == 57);
    CHECK(Same(second.path, "perPlatform"));

    const TextDiagnostic third = diagnostics.Get(2);
    CHECK(third.source.kind == TextSourceKind::None);
    CHECK(third.path.IsEmpty());
    CHECK(Same(third.message, "no resolver"));

    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::UnknownProperty), "UnknownProperty"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::ExcludedProperty), "ExcludedProperty"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::ReadOnly), "ReadOnly"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::InvalidValue), "InvalidValue"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::OutOfRange), "OutOfRange"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::DuplicateKey), "DuplicateKey"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::DuplicateAssignment),
               "DuplicateAssignment"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::HiddenEnumerator), "HiddenEnumerator"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::UnknownType), "UnknownType"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::Unresolved), "Unresolved"));
    CHECK(Same(TextDiagnosticCodeName(TextDiagnosticCode::Unsupported), "Unsupported"));
    CHECK(
        Same(TextDiagnosticCodeName(TextDiagnosticCode::InvalidStructValue), "InvalidStructValue"));
    CHECK(TextDiagnosticCodeName(static_cast<TextDiagnosticCode>(12)).IsEmpty());
}

// 한도까지만 저장하고 넘친 진단은 Omitted로 센다(Add는 성공). 한도를 늘리면 다시 저장하고, 줄이면
// 이미 저장한 진단은 남고 Full이 된다. 많은 진단(한도 기본 256 이상)도 버퍼 재할당을 거쳐 모두
// 남는다.
TEST(TextDiagnostics_LimitStoresFirstAndCountsOmitted) {
    SystemAllocator system;
    TextDiagnostics diagnostics(system);
    diagnostics.SetLimit(2);
    for (uint32 i = 0; i < 5; ++i) {
        REQUIRE(diagnostics.Add(TextDiagnosticCode::InvalidValue, TextSource::Assignment(i), "a",
                                "bad"));
    }

    CHECK(diagnostics.Count() == 2);
    CHECK(diagnostics.Omitted() == 3);
    CHECK(diagnostics.Full());
    CHECK(diagnostics.Get(1).source.sourceIndex == 1);

    diagnostics.SetLimit(3);
    CHECK(!diagnostics.Full());
    REQUIRE(diagnostics.Add(TextDiagnosticCode::OutOfRange, TextSource::Assignment(9), "b", "big"));
    CHECK(diagnostics.Count() == 3);
    CHECK(diagnostics.Get(2).code == TextDiagnosticCode::OutOfRange);

    diagnostics.SetLimit(1);
    CHECK(diagnostics.Full());
    CHECK(diagnostics.Count() == 3);

    diagnostics.Clear();
    CHECK(!diagnostics.HasErrors());
    CHECK(diagnostics.Count() == 0);
    CHECK(diagnostics.Omitted() == 0);
    CHECK(diagnostics.Limit() == 1);

    diagnostics.SetLimit(NumericLimits<uint32>::Max());
    constexpr uint32 many = 600;
    for (uint32 i = 0; i < many; ++i) {
        REQUIRE(diagnostics.Add(TextDiagnosticCode::UnknownProperty, TextSource::Assignment(i),
                                "Downscale.perPlatform[Android]", "unknown property"));
    }

    REQUIRE(diagnostics.Count() == many);
    for (uint32 i = 0; i < many; ++i) {
        const TextDiagnostic item = diagnostics.Get(i);
        CHECK(item.source.sourceIndex == i);
        CHECK(Same(item.path, "Downscale.perPlatform[Android]"));
        CHECK(Same(item.message, "unknown property"));
    }
}

// 할당 실패(텍스트 버퍼·레코드 버퍼 어느 쪽이든)는 allocator 오류를 돌려주고 모음을 바꾸지 않는다
// (이미 돌려준 내용도 그대로다). 정의되지 않은 code·source.kind는 InvalidArgument이고 저장하지
// 않는다.
TEST(TextDiagnostics_FailuresLeaveCollectionUnchanged) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    TextDiagnostics diagnostics(failing);

    // 빈 모음의 첫 Add는 텍스트 버퍼(시도 1)와 레코드 버퍼(시도 2)를 차례로 잡는다.
    for (uint64 attempt = 1; attempt <= 2; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto added = diagnostics.Add(TextDiagnosticCode::ReadOnly, TextSource::Assignment(0),
                                     "locked", "read-only");
        CHECK(failing.Attempts() == attempt);
        failing.DisableFailure();
        REQUIRE(!added);
        CHECK(added.Error() == ErrorCode::OutOfMemory);
        CHECK(diagnostics.Count() == 0);
        CHECK(!diagnostics.HasErrors());
    }

    REQUIRE(diagnostics.Add(TextDiagnosticCode::ReadOnly, TextSource::Assignment(0), "locked",
                            "read-only"));
    REQUIRE(diagnostics.Count() == 1);

    // 텍스트 버퍼를 넘는 path로 재할당을 실패시켜도 저장한 진단은 그대로다.
    char large[512];
    for (char& c : large) {
        c = 'p';
    }

    failing.FailOnAttempt(1);
    auto grown = diagnostics.Add(TextDiagnosticCode::InvalidValue, TextSource::Assignment(1),
                                 StringView(large, sizeof(large)), "x");
    failing.DisableFailure();
    REQUIRE(!grown);
    CHECK(grown.Error() == ErrorCode::OutOfMemory);
    CHECK(diagnostics.Count() == 1);
    CHECK(diagnostics.Omitted() == 0);
    CHECK(Same(diagnostics.Get(0).path, "locked"));
    CHECK(Same(diagnostics.Get(0).message, "read-only"));

    auto badCode = diagnostics.Add(static_cast<TextDiagnosticCode>(12), TextSource{}, "p", "m");
    REQUIRE(!badCode);
    CHECK(badCode.Error() == ErrorCode::InvalidArgument);
    TextSource badSource;
    badSource.kind = static_cast<TextSourceKind>(3);
    auto badKind = diagnostics.Add(TextDiagnosticCode::InvalidValue, badSource, "p", "m");
    REQUIRE(!badKind);
    CHECK(badKind.Error() == ErrorCode::InvalidArgument);
    CHECK(diagnostics.Count() == 1);
}
} // namespace text_diagnostics_tests_file
