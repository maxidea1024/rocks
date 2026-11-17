#include <ReflectionTestFixtures/FieldFlagFixture.generated.h>
#include <Meta/GeneratedType.h>
#include <Platform/Fatal.h>
#include <ReflectionText/TextCodecOptions.h>
#include <ReflectionText/TextUse.h>
#include <TestSupport/Test.h>

// 리플렉션 B9i3a 용도 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.1절): 용도별 출력·입력
// 제외 플래그 표, 생성 스키마 필드(fixture ReflectionTestFixtures/FieldFlagFixture.h)의 용도별
// 판정, 입력 판정의 제외·ReadOnly 순서와 유효 플래그 전파, codec 옵션 범위 검사.
using namespace rocks;
using namespace rocks::reflection_text;
using reflection_fixture::FlagHolder;

namespace {

constexpr uint32 Transient = static_cast<uint32>(FieldFlags::Transient);
constexpr uint32 ReadOnly = static_cast<uint32>(FieldFlags::ReadOnly);
constexpr uint32 EditorOnly = static_cast<uint32>(FieldFlags::EditorOnly);
constexpr uint32 Required = static_cast<uint32>(FieldFlags::Required);
constexpr uint32 Duplicate = static_cast<uint32>(FieldFlags::DuplicateTransient);
constexpr uint32 NonPie = static_cast<uint32>(FieldFlags::NonPIEDuplicateTransient);
constexpr uint32 SaveGame = static_cast<uint32>(FieldFlags::SaveGame);
constexpr uint32 TextExport = static_cast<uint32>(FieldFlags::TextExportTransient);

constexpr TextUse Uses[] = {TextUse::Edit, TextUse::Copy, TextUse::PieCopy, TextUse::Report};
constexpr TextUse InputUses[] = {TextUse::Edit, TextUse::Copy, TextUse::PieCopy};

uint32 InputFlags(TextUse use) noexcept {
    auto flags = TextInputExcludedFlags(use);
    if (!flags) {
        platform::Fatal("input use expected");
    }

    return flags.Value();
}

uint32 FieldFlagsOf(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.flags;
        }
    }

    platform::Fatal("fixture field missing");
}

} // namespace

// 용도별 제외 플래그가 10.1절 표와 같다: 출력은 Edit = Transient·NonPIE, Copy = Edit + Duplicate·
// TextExport, PieCopy = Transient·Duplicate·TextExport, Report = Transient. 입력은 Edit =
// Transient, Copy·PieCopy = 출력과 같음, Report = InvalidArgument. 정의되지 않은 값은
// IsValidTextUse가 거짓이고 입력 플래그 조회가 InvalidArgument다.
TEST(TextUse_ExclusionTableMatchesSpec) {
    CHECK(TextOutputExcludedFlags(TextUse::Edit) == (Transient | NonPie));
    CHECK(TextOutputExcludedFlags(TextUse::Copy) == (Transient | NonPie | Duplicate | TextExport));
    CHECK(TextOutputExcludedFlags(TextUse::PieCopy) == (Transient | Duplicate | TextExport));
    CHECK(TextOutputExcludedFlags(TextUse::Report) == Transient);

    CHECK(InputFlags(TextUse::Edit) == Transient);
    CHECK(InputFlags(TextUse::Copy) == TextOutputExcludedFlags(TextUse::Copy));
    CHECK(InputFlags(TextUse::PieCopy) == TextOutputExcludedFlags(TextUse::PieCopy));
    auto report = TextInputExcludedFlags(TextUse::Report);
    REQUIRE(!report);
    CHECK(report.Error() == ErrorCode::InvalidArgument);

    for (TextUse use : Uses) {
        CHECK(IsValidTextUse(use));
        // 텍스트 용도와 무관한 플래그는 어떤 용도에서도 빼지 않는다.
        CHECK(!IsTextOutputExcluded(use, EditorOnly | Required | SaveGame | ReadOnly));
    }

    const TextUse undefined = static_cast<TextUse>(4);
    CHECK(!IsValidTextUse(undefined));
    auto undefinedInput = TextInputExcludedFlags(undefined);
    REQUIRE(!undefinedInput);
    CHECK(undefinedInput.Error() == ErrorCode::InvalidArgument);
}

// 생성 스키마의 필드 플래그로 판정한다(FlagHolder): duplicateOnly(DuplicateTransient)는 Edit·Report
// 출력에 나오고 Copy·PieCopy에서 빠진다. instance(NonPIEDuplicateTransient)는 Edit·Copy 출력에서
// 빠지고 PieCopy·Report에 나오며 Edit 입력은 받는다(인스턴스 고유 값도 도구가 고친다). textOnly
// (TextExportTransient)는 Edit·Report 출력에 나오고 Copy·PieCopy에서 빠지며 Edit 입력은 받는다.
// score(SaveGame)·plain은 모든 용도에 나온다.
TEST(TextUse_FixtureFieldsFollowUseTable) {
    const TypeSchema& schema = GeneratedType<FlagHolder>::Schema();
    const uint32 duplicateOnly = FieldFlagsOf(schema, "duplicateOnly");
    const uint32 instance = FieldFlagsOf(schema, "instance");
    const uint32 textOnly = FieldFlagsOf(schema, "textOnly");
    const uint32 score = FieldFlagsOf(schema, "score");
    const uint32 plain = FieldFlagsOf(schema, "plain");

    CHECK(!IsTextOutputExcluded(TextUse::Edit, duplicateOnly));
    CHECK(IsTextOutputExcluded(TextUse::Copy, duplicateOnly));
    CHECK(IsTextOutputExcluded(TextUse::PieCopy, duplicateOnly));
    CHECK(!IsTextOutputExcluded(TextUse::Report, duplicateOnly));

    CHECK(IsTextOutputExcluded(TextUse::Edit, instance));
    CHECK(IsTextOutputExcluded(TextUse::Copy, instance));
    CHECK(!IsTextOutputExcluded(TextUse::PieCopy, instance));
    CHECK(!IsTextOutputExcluded(TextUse::Report, instance));

    CHECK(!IsTextOutputExcluded(TextUse::Edit, textOnly));
    CHECK(IsTextOutputExcluded(TextUse::Copy, textOnly));
    CHECK(IsTextOutputExcluded(TextUse::PieCopy, textOnly));
    CHECK(!IsTextOutputExcluded(TextUse::Report, textOnly));

    for (TextUse use : Uses) {
        CHECK(!IsTextOutputExcluded(use, score));
        CHECK(!IsTextOutputExcluded(use, plain));
    }

    // Edit 입력은 출력에서 빠지는 NonPIE·TextExport 필드도 받는다. Copy·PieCopy 입력은 출력과 같다.
    CHECK(ClassifyTextInput(TextUse::Edit, duplicateOnly) == TextFieldInput::Accept);
    CHECK(ClassifyTextInput(TextUse::Edit, instance) == TextFieldInput::Accept);
    CHECK(ClassifyTextInput(TextUse::Edit, textOnly) == TextFieldInput::Accept);
    CHECK(ClassifyTextInput(TextUse::Copy, duplicateOnly) == TextFieldInput::Excluded);
    CHECK(ClassifyTextInput(TextUse::Copy, instance) == TextFieldInput::Excluded);
    CHECK(ClassifyTextInput(TextUse::Copy, textOnly) == TextFieldInput::Excluded);
    CHECK(ClassifyTextInput(TextUse::PieCopy, duplicateOnly) == TextFieldInput::Excluded);
    CHECK(ClassifyTextInput(TextUse::PieCopy, instance) == TextFieldInput::Accept);
    CHECK(ClassifyTextInput(TextUse::PieCopy, textOnly) == TextFieldInput::Excluded);
    for (TextUse use : InputUses) {
        CHECK(ClassifyTextInput(use, score) == TextFieldInput::Accept);
        CHECK(ClassifyTextInput(use, plain) == TextFieldInput::Accept);
    }
}

// 입력 판정: 제외가 ReadOnly보다 먼저이고(Transient + ReadOnly는 Excluded), ReadOnly만이면
// ReadOnly다. 판정은 유효 플래그를 본다 — 바깥 struct의 Transient·ReadOnly는 PropagateFieldFlags로
// 멤버에 전파되어 멤버도 같은 판정을 받는다. 전파되지 않는 DuplicateTransient struct의 멤버는 그
// 자리에서는 제외가 아니므로 codec은 바깥 단계에서 멈춰 부분 트리를 통째로 뺀다(TextUse.h 판정
// 규칙).
TEST(TextUse_InputClassificationOrderAndPropagation) {
    for (TextUse use : InputUses) {
        CHECK(ClassifyTextInput(use, 0) == TextFieldInput::Accept);
        CHECK(ClassifyTextInput(use, ReadOnly) == TextFieldInput::ReadOnly);
        CHECK(ClassifyTextInput(use, ReadOnly | EditorOnly) == TextFieldInput::ReadOnly);
        CHECK(ClassifyTextInput(use, Transient) == TextFieldInput::Excluded);
        CHECK(ClassifyTextInput(use, Transient | ReadOnly) == TextFieldInput::Excluded);

        const uint32 underTransient = PropagateFieldFlags(Transient, 0);
        CHECK(ClassifyTextInput(use, underTransient) == TextFieldInput::Excluded);
        CHECK(IsTextOutputExcluded(use, underTransient));
        const uint32 underReadOnly = PropagateFieldFlags(ReadOnly, 0);
        CHECK(ClassifyTextInput(use, underReadOnly) == TextFieldInput::ReadOnly);
    }

    CHECK(ClassifyTextInput(TextUse::Copy, Duplicate | ReadOnly) == TextFieldInput::Excluded);
    const uint32 underDuplicate = PropagateFieldFlags(Duplicate, 0);
    CHECK(!IsTextOutputExcluded(TextUse::Copy, underDuplicate));
    CHECK(IsTextOutputExcluded(TextUse::Copy, Duplicate));
}

// codec 옵션 범위: 기본값은 유효하고, 정의되지 않은 use·mapMerge와 0 한도·MaxArrayElements를 넘는
// 원소 상한은 InvalidArgument다. Report 용도 자체는 옵션 검사에서 거부하지 않는다(출력에 쓴다).
TEST(TextCodecOptions_ValidateRanges) {
    TextCodecOptions defaults;
    CHECK(defaults.use == TextUse::Edit);
    CHECK(defaults.resolver == nullptr);
    CHECK(defaults.names == nullptr);
    CHECK(defaults.mapMerge == MapMergeMode::Replace);
    CHECK(defaults.maxDiagnostics == DefaultMaxTextDiagnostics);
    CHECK(defaults.maxElements == MaxArrayElements);
    CHECK(ValidateTextCodecOptions(defaults));

    TextCodecOptions report;
    report.use = TextUse::Report;
    CHECK(ValidateTextCodecOptions(report));

    TextCodecOptions merge;
    merge.mapMerge = MapMergeMode::MergeByKey;
    merge.maxDiagnostics = 1;
    merge.maxInputBytes = 1;
    merge.maxJsonDepth = 1;
    merge.maxElements = 1;
    CHECK(ValidateTextCodecOptions(merge));

    auto rejects = [](TextCodecOptions options) noexcept {
        auto checked = ValidateTextCodecOptions(options);
        return !checked && checked.Error() == ErrorCode::InvalidArgument;
    };

    TextCodecOptions bad;
    bad.use = static_cast<TextUse>(4);
    CHECK(rejects(bad));
    bad = {};
    bad.mapMerge = static_cast<MapMergeMode>(2);
    CHECK(rejects(bad));
    bad = {};
    bad.maxDiagnostics = 0;
    CHECK(rejects(bad));
    bad = {};
    bad.maxInputBytes = 0;
    CHECK(rejects(bad));
    bad = {};
    bad.maxJsonDepth = 0;
    CHECK(rejects(bad));
    bad = {};
    bad.maxElements = 0;
    CHECK(rejects(bad));
    bad = {};
    bad.maxElements = MaxArrayElements + 1;
    CHECK(rejects(bad));
}
