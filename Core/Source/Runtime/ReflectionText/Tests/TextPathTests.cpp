#include "TextTestSupport.h"

// 리플렉션 B9i3b 경로 표기 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.5·10.1·10.4절, fixture
// ReflectionTestFixtures/TextFixture.h): 경로의 자리(필드·원소·Map 값·Set 원소, 단계 플래그 합),
// 엄격성(모르는 이름·대소문자 무시·문법·숨김 열거자 key·NFC가 아닌 Name key·ClassRef·Soft key
// hook), 용도별 입력·출력 제외(경로 단계 전체), 대입 분해(`[...]`·따옴표 밖 첫 `=`), 입력 byte·진단
// 한도.
using namespace rocks;
namespace text_path_tests_file {

using namespace rocks::test::text;

namespace {

constexpr uint32 ReadOnlyFlag = static_cast<uint32>(FieldFlags::ReadOnly);
constexpr uint32 DuplicateFlag = static_cast<uint32>(FieldFlags::DuplicateTransient);

// path를 해석해 다시 쓴 텍스트가 expected인지 본다.
bool PathRoundTrip(TextCodecContext& context, const TypeLease& type, StringView path,
                   StringView expected) noexcept {
    TextPath resolved;
    auto made = ResolveTextPath(context, type, path, TextSource{}, resolved);
    if (!made || !made.Value()) {
        return false;
    }

    String text(context.GetAllocator());
    auto formatted = FormatTextPath(context, type, resolved.path.View(), text);
    return formatted && formatted.Value() && text.View() == expected;
}

} // namespace

// 경로의 자리: 필드 이름은 대소문자 무시이고 출력은 실효 도구 이름이다. struct 멤버·원소·Map 값·Set
// 원소 자리와 단계 플래그의 합(조상의 DuplicateTransient 포함)을 돌려준다. key는 값 표기 규칙이다 —
// enum key는 도구 이름, ClassRef key는 KeyHook(타입 도구 이름), Soft key는 resolver가 없으면 36자
// AssetId.
TEST(TextPath_ResolvesSlotsAndKeys) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TypeLease& holder = setup.holder;

    const TextSlot member = SlotOf(context, holder, "Origin.X");
    REQUIRE(member.field);
    CHECK(StringView(member.field->name.Data(), member.field->name.Size()) == "x");
    CHECK(member.part == TextSlotPart::Field);
    CHECK(SlotValueType(member).type.kind == ValueKind::Int32);
    const TextSlot copied = SlotOf(context, holder, "copied.y");
    CHECK((copied.flags & DuplicateFlag) != 0);
    CHECK((SlotOf(context, holder, "locked").flags & ReadOnlyFlag) != 0);
    CHECK(SlotOf(context, holder, "byQuality[high]").part == TextSlotPart::MapValue);
    CHECK(SlotKeyType(SlotOf(context, holder, "byQuality")).enumSchema != nullptr);

    CHECK(PathRoundTrip(context, holder, "ORIGIN.x", "origin.x"));
    CHECK(PathRoundTrip(context, holder, "byQuality[tq_high]", "byQuality[High]"));
    CHECK(PathRoundTrip(context, holder, "byClass[text.renamed]", "byClass[Text.Renamed]"));
    CHECK(PathRoundTrip(context, holder, "byClass[TextKindDerived]", "byClass[TextKindDerived]"));
    CHECK(PathRoundTrip(context, holder, "bySoft[11112222-3333-4444-5555-666677778888]",
                        "bySoft[11112222-3333-4444-5555-666677778888]"));
    CHECK(PathRoundTrip(context, holder, "scores[\"a,b\"]", "scores[\"a,b\"]"));
    CHECK(PathRoundTrip(context, holder, "byName[Rock].y", "byName[Rock].y"));
    CHECK(PathRoundTrip(context, holder, "points[2].x", "points[2].x"));
    CHECK(SlotOf(context, holder, "tags[Rock]").part == TextSlotPart::SetElement);
}

// 경로 엄격성: 모르는 이름 UnknownProperty, 문법·key 형식 InvalidValue, 숨김 열거자 key
// HiddenEnumerator, NFC가 아닌 Name key InvalidValue, 미등록 ClassRef key UnknownType, resolver
// 없는 경로 Soft key Unsupported. 진단 경로는 입력 텍스트다.
TEST(TextPath_StrictnessDiagnostics) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TypeLease& holder = setup.holder;
    CHECK(PathRejects(context, holder, "nope", TextDiagnosticCode::UnknownProperty));
    CHECK(PathRejects(context, holder, "origin.z", TextDiagnosticCode::UnknownProperty));
    CHECK(PathRejects(context, holder, "origin[", TextDiagnosticCode::InvalidValue));
    CHECK(PathRejects(context, holder, "numbers[01]", TextDiagnosticCode::InvalidValue));
    CHECK(PathRejects(context, holder, "byQuality[Medium]", TextDiagnosticCode::InvalidValue));
    CHECK(PathRejects(context, holder, "byQuality[Secret]", TextDiagnosticCode::HiddenEnumerator));
    CHECK(PathRejects(context, holder, "tags[Cafe\xCC\x81]", TextDiagnosticCode::InvalidValue));
    CHECK(PathRejects(context, holder, "byClass[Missing]", TextDiagnosticCode::UnknownType));
    CHECK(PathRejects(context, holder, "byClass[TextStranger]", TextDiagnosticCode::InvalidValue));
    CHECK(PathRejects(context, holder, "bySoft[\"/Game/Rock.Rock\"]",
                      TextDiagnosticCode::Unsupported));
    CHECK(context.Diagnostics().Get(0).source.sourceIndex == 3);
}

// 용도별 제외를 경로 전체에 적용한다: Edit 입력은 Transient만 빼고(NonTryout·TextExport 필드는
// 받는다) ReadOnly는 판정하지 않는다(대입 묶음 몫). Copy 입력은 DuplicateTransient struct의
// 멤버(조상 단계)와 TextExport·NonTryout을 뺀다. TryoutCopy는 NonTryout을 받는다. Report는 입력에
// 쓸 수 없다(InvalidArgument). 출력(FormatTextPath)은 용도의 출력 제외가 경로 어느 단계에 있으면
// 쓰지 않고 false다.
TEST(TextPath_UseExclusionCoversWholePath) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    const TypeLease& holder = setup.holder;
    {
        TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
        CHECK(PathRejects(context, holder, "cache", TextDiagnosticCode::ExcludedProperty));
        SlotOf(context, holder, "exportOnly");
        SlotOf(context, holder, "instance");
        SlotOf(context, holder, "copied.x");
        SlotOf(context, holder, "locked");
    }

    setup.options.use = TextUse::Copy;
    {
        TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
        CHECK(PathRejects(context, holder, "copied.x", TextDiagnosticCode::ExcludedProperty));
        CHECK(PathRejects(context, holder, "copied", TextDiagnosticCode::ExcludedProperty));
        CHECK(PathRejects(context, holder, "exportOnly", TextDiagnosticCode::ExcludedProperty));
        CHECK(PathRejects(context, holder, "instance", TextDiagnosticCode::ExcludedProperty));
        SlotOf(context, holder, "origin.x");
    }

    setup.options.use = TextUse::TryoutCopy;
    {
        TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
        SlotOf(context, holder, "instance");
        CHECK(PathRejects(context, holder, "copied.y", TextDiagnosticCode::ExcludedProperty));
    }

    // 출력: 경로는 Edit 문맥으로 해석해 두고 용도별 문맥으로 쓴다.
    setup.options.use = TextUse::Edit;
    TextCodecContext edit(setup.types, setup.options, setup.diagnostics, allocator);
    TextPath copiedX;
    TextPath instance;
    REQUIRE(ResolveTextPath(edit, holder, "copied.x", TextSource{}, copiedX));
    REQUIRE(ResolveTextPath(edit, holder, "instance", TextSource{}, instance));
    String text(allocator);
    auto shown = FormatTextPath(edit, holder, copiedX.path.View(), text);
    REQUIRE(shown);
    CHECK(shown.Value());
    CHECK(text.View() == "copied.x");
    auto hidden = FormatTextPath(edit, holder, instance.path.View(), text);
    REQUIRE(hidden);
    CHECK(!hidden.Value());
    CHECK(text.View() == "copied.x");

    TextCodecOptions copyOptions = setup.options;
    copyOptions.use = TextUse::Copy;
    TextDiagnostics copyDiagnostics(allocator);
    TextCodecContext copy(setup.types, copyOptions, copyDiagnostics, allocator);
    auto copyShown = FormatTextPath(copy, holder, copiedX.path.View(), text);
    REQUIRE(copyShown);
    CHECK(!copyShown.Value());

    TextCodecOptions reportOptions = setup.options;
    reportOptions.use = TextUse::Report;
    TextDiagnostics reportDiagnostics(allocator);
    TextCodecContext report(setup.types, reportOptions, reportDiagnostics, allocator);
    auto reported = FormatTextPath(report, holder, instance.path.View(), text);
    REQUIRE(reported);
    CHECK(reported.Value());
    TextPath rejected;
    auto input = ResolveTextPath(report, holder, "origin.x", TextSource{}, rejected);
    REQUIRE(!input);
    CHECK(input.Error() == ErrorCode::InvalidArgument);
}

// 대입 분해: `[...]`와 key 따옴표 밖의 첫 `=`에서 나누고(값 안의 `=`는 값), 따옴표 안의 `\"`는
// 건너뛴다. `=`가 없으면 false다.
TEST(TextPath_SplitAssignment) {
    StringView path;
    StringView value;
    CHECK(SplitTextAssignment("label=a=b", path, value));
    CHECK(path == "label");
    CHECK(value == "a=b");
    CHECK(SplitTextAssignment("scores[\"a=b\"]=1", path, value));
    CHECK(path == "scores[\"a=b\"]");
    CHECK(value == "1");
    CHECK(SplitTextAssignment("scores[\"x\\\"]=\"]=2", path, value));
    CHECK(path == "scores[\"x\\\"]=\"]");
    CHECK(value == "2");
    CHECK(SplitTextAssignment("scores[k=v]=3", path, value));
    CHECK(path == "scores[k=v]");
    CHECK(SplitTextAssignment("numbers=", path, value));
    CHECK(path == "numbers");
    CHECK(value.IsEmpty());
    path = "kept";
    CHECK(!SplitTextAssignment("numbers", path, value));
    CHECK(path == "kept");
}

// 한도: 입력 byte는 문맥의 경로·값 텍스트 누계이고 넘으면 그 호출이 CapacityExceeded(누계·진단
// 불변)다. 진단 한도에 차면 목록 해석이 남은 원소를 멈추고 실패한다.
TEST(TextPath_InputAndDiagnosticLimits) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    setup.options.maxInputBytes = 10;
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    TextPath resolved;
    REQUIRE(ResolveTextPath(context, setup.holder, "origin.x", TextSource{}, resolved));
    CHECK(context.ConsumedInputBytes() == 8);
    TextValue value(allocator);
    auto parsed = ParseTextValue(context, resolved.slot, "123", TextSource{}, "origin.x", value);
    REQUIRE(!parsed);
    CHECK(parsed.Error() == ErrorCode::CapacityExceeded);
    CHECK(context.ConsumedInputBytes() == 8);
    CHECK(setup.diagnostics.Count() == 0);
    REQUIRE(ParseTextValue(context, resolved.slot, "12", TextSource{}, "origin.x", value));
    CHECK(context.ConsumedInputBytes() == 10);

    TextCodecOptions few;
    few.maxDiagnostics = 2;
    TextDiagnostics diagnostics(allocator);
    TextCodecContext limited(setup.types, few, diagnostics, allocator);
    const TextSlot numbers = SlotOf(limited, setup.holder, "numbers");
    auto many = ParseTextValue(limited, numbers, "x,y,z,1", TextSource{}, "numbers", value);
    REQUIRE(many);
    CHECK(!many.Value());
    CHECK(diagnostics.Count() == 2);
    CHECK(diagnostics.Full());
    CHECK(value.shape == TextValueShape::Empty);

    TextCodecOptions invalid;
    invalid.maxElements = 0;
    TextDiagnostics unused(allocator);
    TextCodecContext broken(setup.types, invalid, unused, allocator);
    auto start = broken.Start();
    REQUIRE(!start);
    CHECK(start.Error() == ErrorCode::InvalidArgument);
}
} // namespace text_path_tests_file
