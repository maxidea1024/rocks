#include "TextAssignTestSupport.h"

#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/TextFieldFixture.generated.h>
#include <ReflectionText/TextJson.h>

// 리플렉션 B9l Text 값의 텍스트·JSON 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.4·13절,
// fixture ReflectionTestFixtures/TextFieldFixture.h): 명령행은 Invariant 원문만(빈 텍스트는 비어
// 있음, Localized 출력은 Unsupported), JSON은 문자열 = Invariant·객체 {"id","revision","source"} =
// Localized·null = 비어 있음의 export·import 왕복과 거부 진단.
using namespace rocks;
namespace text_field_text_tests_file {

using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::TextFieldHolder;

namespace {

constexpr TextId CaptionId{0x5446, 7};

// TextFieldHolder 객체 Registry다(소멸: Registry → 등록 → 타입).
struct TextObjects {
    TextObjects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x5446, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~TextObjects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<TextFieldHolder> Make() noexcept {
        auto created =
            objects.NewObject<TextFieldHolder>(access, GeneratedType<TextFieldHolder>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<TextFieldHolder>{};
    }

    Result<void> Apply(ObjectHandle object, Span<const TextAssignment> inputs) noexcept {
        diagnostics.Clear();
        auto prepared = PrepareAssignments(AssignmentTarget::Live(objects, access, object), inputs,
                                           options, diagnostics);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        auto published = prepared.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

    template <class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<TextFieldHolder>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    reflection_text::TextDiagnostics diagnostics;
    TextCodecOptions options;
};

Result<void> Export(const AssignmentTarget& target, const TextCodecOptions& options,
                    StringBuilderBase& out) noexcept {
    out.Clear();
    JsonWriter writer(out);
    if (auto written = ExportJson(target, writer, options); !written) {
        return written;
    }

    return writer.Finish();
}

bool HasForm(const Text& text, TextPersistentForm form, StringView source, TextId id = {},
             uint64 revision = 0) noexcept {
    TextPersistentView view;
    return text.TryGetPersistentForm(view) && view.form == form && view.source == source &&
           view.id == id && view.sourceRevision == revision;
}

} // namespace

// 명령행 대입은 Invariant 원문이다(필드·Array 원소·Map 값·struct 멤버). 빈 텍스트는 비어 있음이다.
TEST(TextFieldText_CommandLineAssignsInvariant) {
    TextObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("title", "Hello"),        Set("lines", "a,b", 1),
                                     Set("byKey[3]", "three", 2),  Set("caption.label", "cap", 3),
                                     Set("fixed[1]", "second", 4), Set("note", "", 5)};
    REQUIRE(fixture.Apply(object, inputs));
    fixture.With(object, [](TextFieldHolder& value) noexcept {
        CHECK(HasForm(value.title, TextPersistentForm::Invariant, "Hello"));
        REQUIRE(value.lines.Size() == 2);
        CHECK(HasForm(value.lines[1], TextPersistentForm::Invariant, "b"));
        REQUIRE(value.byKey.Find(3u));
        CHECK(HasForm(*value.byKey.Find(3u), TextPersistentForm::Invariant, "three"));
        CHECK(HasForm(value.caption.label, TextPersistentForm::Invariant, "cap"));
        CHECK(HasForm(value.fixed[1], TextPersistentForm::Invariant, "second"));
        CHECK(!value.note.IsValid());
    });
}

// JSON: 비어 있음은 null, Invariant는 문자열, Localized는 {"id","revision","source"}이고 그 JSON을
// 새 객체에 import하면 다시 쓴 텍스트가 같다.
TEST(TextFieldText_JsonRoundTripsForms) {
    TextObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    fixture.With(object, [&](TextFieldHolder& value) noexcept {
        auto localized =
            Text::Localized(fixture.objects.ObjectAllocator(), CaptionId, 4, "Caption");
        REQUIRE(localized);
        value.title = Move(localized.Value());
        auto invariant = Text::Invariant(fixture.objects.ObjectAllocator(), "plain");
        REQUIRE(invariant);
        value.caption.label = Move(invariant.Value());
    });

    StringBuilder<1024> text(fixture.system);
    REQUIRE(Export(AssignmentTarget::Live(fixture.objects, fixture.access, object), fixture.options,
                   text));
    CHECK(text.View().Contains("\"revision\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"Caption\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"plain\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("null", StringComparison::CaseSensitive));

    auto copy = fixture.Make();
    JsonDocument document(fixture.system);
    REQUIRE(document.Parse(text.View()).Succeeded());
    fixture.diagnostics.Clear();
    auto imported = ImportJson(
        document.Root(), AssignmentTarget::Live(fixture.objects, fixture.access, copy.Handle()),
        fixture.options, fixture.diagnostics);
    REQUIRE(imported);
    REQUIRE(imported.Value().Publish());
    fixture.With(copy.Handle(), [](TextFieldHolder& value) noexcept {
        CHECK(HasForm(value.title, TextPersistentForm::Localized, "Caption", CaptionId, 4));
        CHECK(HasForm(value.caption.label, TextPersistentForm::Invariant, "plain"));
        CHECK(!value.note.IsValid());
    });
    StringBuilder<1024> again(fixture.system);
    REQUIRE(Export(AssignmentTarget::Live(fixture.objects, fixture.access, copy.Handle()),
                   fixture.options, again));
    CHECK(again.View() == text.View());
}

// JSON 거부: 모르는 키·빠진 키·id 0·revision 0·숫자 값은 InvalidValue 진단이다. 명령행 출력은
// Localized를 쓸 수 없다(Unsupported).
TEST(TextFieldText_RejectionsAndLocalizedCommandLineOutput) {
    TextObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const char* const inputs[] = {
        "{\"title\":{\"id\":\"00000000-0000-0000-0000-000000000000\",\"revision\":1,\"source\":"
        "\"a\"}}",
        "{\"title\":{\"id\":\"00000000-0000-5446-0000-000000000007\",\"revision\":0,\"source\":"
        "\"a\"}}",
        "{\"title\":{\"id\":\"00000000-0000-5446-0000-000000000007\",\"source\":\"a\"}}",
        "{\"title\":{\"id\":\"00000000-0000-5446-0000-000000000007\",\"revision\":1,\"source\":"
        "\"a\",\"x\":1}}",
        "{\"title\":7}"};
    for (const char* input : inputs) {
        JsonDocument document(fixture.system);
        REQUIRE(document.Parse(StringView(input)).Succeeded());
        fixture.diagnostics.Clear();
        auto imported = ImportJson(document.Root(),
                                   AssignmentTarget::Live(fixture.objects, fixture.access, object),
                                   fixture.options, fixture.diagnostics);
        CHECK(!imported);
        REQUIRE(fixture.diagnostics.Count() >= 1);
        CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
    }

    fixture.With(object, [](TextFieldHolder& value) noexcept { CHECK(!value.title.IsValid()); });

    FieldValue localized(fixture.system);
    localized.kind = ValueKind::Text;
    auto made = Text::Localized(fixture.system, CaptionId, 1, "x");
    REQUIRE(made);
    localized.text = Move(made.Value());
    TextCodecContext context(fixture.types, fixture.options, fixture.diagnostics, fixture.system);
    StringBuilder<64> out(fixture.system);
    auto formatted = FormatTextScalar(
        context, TextScalarType{ValueType{ValueKind::Text, {}}, nullptr}, localized, out);
    CHECK(!formatted && formatted.Error() == ErrorCode::Unsupported);
}
} // namespace text_field_text_tests_file
