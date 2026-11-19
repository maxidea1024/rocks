#include "TextAssignTestSupport.h"

#include <ReflectionTestFixtures/BitfieldFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionText/TextJson.h>

// 리플렉션 B9m bitfield bool의 텍스트·JSON 시험이다(Docs/Design/ReflectionExtensions.ko.md 14절 —
// 텍스트는 bool과 같다, fixture ReflectionTestFixtures/BitfieldFixture.h): 명령행 대입(클래스 필드·
// 상속·private 필드·struct 멤버 — 값 단위 Detach·Attach를 거친다)과 이웃 비트 보존, JSON 왕복.
using namespace rocks;
using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::BitfieldChild;

namespace {

// BitfieldChild 객체 Registry다(소멸: Registry → 등록 → 타입).
struct BitfieldObjects {
    BitfieldObjects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x4254, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~BitfieldObjects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<BitfieldChild> Make() noexcept {
        auto created =
            objects.NewObject<BitfieldChild>(access, GeneratedType<BitfieldChild>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<BitfieldChild>{};
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
        auto borrowed = objects.Resolve<BitfieldChild>(access, object);
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

} // namespace

// 명령행 대입은 bool과 같고 같은 바이트의 이웃(NATIVE bitfield·다른 bitfield·일반 멤버)을 보존한다.
// ReadOnly bitfield는 진단이다.
TEST(TextBitfield_CommandLineAssignsBits) {
    BitfieldObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    fixture.With(object, [](BitfieldChild& value) noexcept {
        value.cache = 45;
        value.flags.spare = 37;
        value.flags.plain = 200;
        value.flags.first = 1;
    });
    const TextAssignment inputs[] = {Set("on", "true"),
                                     Set("extra", "true", 1),
                                     Set("hidden", "true", 2),
                                     Set("flags.second", "true", 3),
                                     Set("flags.first", "false", 4),
                                     Set("big", "true", 5)};
    REQUIRE(fixture.Apply(object, inputs));
    fixture.With(object, [](BitfieldChild& value) noexcept {
        CHECK(value.on == 1 && value.extra == 1 && value.Hidden() && value.big == 1);
        CHECK(value.cache == 45);
        CHECK(value.flags.first == 0 && value.flags.second == 1 && value.flags.spare == 37 &&
              value.flags.plain == 200);
    });

    const TextAssignment locked[] = {Set("locked", "true")};
    auto rejected = fixture.Apply(object, locked);
    CHECK(!rejected);
    REQUIRE(fixture.diagnostics.Count() >= 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::ReadOnly);
}

// JSON은 bool이고 새 객체에 import하면 다시 쓴 텍스트가 같다.
TEST(TextBitfield_JsonRoundTrips) {
    BitfieldObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    fixture.With(object, [](BitfieldChild& value) noexcept {
        value.on = 1;
        value.extra = 1;
        value.flags.widest = 1;
        value.flags.plain = 7;
        REQUIRE(value.list.TryPushBack(reflection_fixture::BitfieldFlags{}));
        value.list[0].wider = 1;
    });

    StringBuilder<2048> text(fixture.system);
    REQUIRE(Export(AssignmentTarget::Live(fixture.objects, fixture.access, object), fixture.options,
                   text));
    CHECK(text.View().Contains("\"on\":true", StringComparison::CaseSensitive) ||
          text.View().Contains("\"on\": true", StringComparison::CaseSensitive));

    auto copy = fixture.Make();
    JsonDocument document(fixture.system);
    REQUIRE(document.Parse(text.View()).Succeeded());
    fixture.diagnostics.Clear();
    auto imported = ImportJson(
        document.Root(), AssignmentTarget::Live(fixture.objects, fixture.access, copy.Handle()),
        fixture.options, fixture.diagnostics);
    REQUIRE(imported);
    REQUIRE(imported.Value().Publish());
    fixture.With(copy.Handle(), [](BitfieldChild& value) noexcept {
        CHECK(value.on == 1 && value.extra == 1 && value.flags.widest == 1 &&
              value.flags.plain == 7);
        REQUIRE(value.list.Size() == 1);
        CHECK(value.list[0].wider == 1);
    });
    StringBuilder<2048> again(fixture.system);
    REQUIRE(Export(AssignmentTarget::Live(fixture.objects, fixture.access, copy.Handle()),
                   fixture.options, again));
    CHECK(again.View() == text.View());
}
