#include "TextAssignTestSupport.h"

#include <ReflectionTestFixtures/EventFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <ReflectionText/TextJson.h>
#include <ReflectionText/TextLines.h>
#include <ReflectionText/TextVisit.h>

// 이벤트 필드의 텍스트 경로 시험이다(Docs/Design/EventReflection.ko.md 3.1절 — EV2b): JSON·한 줄
// 텍스트 내보내기는 이벤트 필드를 통째로 빼고(값 필드는 그대로), JSON 가져오기에서 이벤트 필드 키는
// "no JSON form" 진단이다.
using namespace rocks;
namespace text_event_field_tests_file {

using namespace rocks::reflection_text;
using reflection_fixture::EventPublisher;

namespace {

struct EventObjects {
    EventObjects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x54455654, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~EventObjects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<EventPublisher> Make() noexcept {
        auto created =
            objects.NewObject<EventPublisher>(access, GeneratedType<EventPublisher>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<EventPublisher>{};
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
};

bool Contains(StringView text, StringView part) noexcept {
    return text.Contains(part, StringComparison::CaseSensitive);
}

} // namespace

TEST(TextEventField_ExportSkipsEvents) {
    EventObjects fixture;
    auto root = fixture.Make();
    {
        auto borrow =
            fixture.objects.ResolveForWrite<EventPublisher>(fixture.access, root.Handle());
        REQUIRE(borrow);
        auto value = borrow.Value().Get();
        REQUIRE(value);
        value.Value()->count = 12;
        REQUIRE(value.Value()->changed.TryAddLambda([](int32) noexcept {}));
    }

    StringBuilder<> json(fixture.system);
    JsonWriter writer(json);
    REQUIRE(ExportJson(AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle()),
                       writer, fixture.options));
    REQUIRE(writer.Finish());
    const StringView text = json.View();
    CHECK(Contains(text, "\"count\"") && Contains(text, "\"label\""));
    CHECK(!Contains(text, "changed") && !Contains(text, "pinged") && !Contains(text, "placed"));

    String lines(fixture.system);
    REQUIRE(ExportLines(AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle()),
                        lines, fixture.options));
    CHECK(Contains(lines.View(), "count") && !Contains(lines.View(), "changed"));
}

TEST(TextEventField_JsonImportReportsEventKey) {
    EventObjects fixture;
    auto root = fixture.Make();
    JsonDocument document(fixture.system);
    REQUIRE(document.Parse("{\"count\": 4, \"changed\": 1}").Succeeded());
    fixture.diagnostics.Clear();
    auto prepared = ImportJson(
        document.Root(), AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle()),
        fixture.options, fixture.diagnostics);
    CHECK(fixture.diagnostics.HasErrors());
    CHECK(!prepared || fixture.diagnostics.Count() == 1);
}

} // namespace text_event_field_tests_file
