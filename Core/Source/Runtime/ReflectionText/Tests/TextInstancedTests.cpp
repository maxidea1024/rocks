#include "TextAssignTestSupport.h"

#include <ReflectionTestFixtures/InstancedStructFixture.generated.h>
#include <ReflectionTestFixtures/StructInheritanceFixture.generated.h>
#include <Core/Text/NamePool.h>
#include <ReflectionText/TextJson.h>

// 리플렉션 B9j InstancedStruct의 텍스트·JSON 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.4·
// 11절, fixture ReflectionTestFixtures/InstancedStructFixture.h): 명령행 형 바꾸기(`f=<타입 도구
// 이름>`)·비우기(`f=none`)·멤버 대입(`f.m=v` — 같은 묶음의 앞 형 바꾸기 위에서 해석), 원소·Map 값
// 자리, 거부 진단(모르는 타입·ReadOnly·깊이 합·빈 값의 멤버), JSON `_structType` export·import
// 왕복.
using namespace rocks;
using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::InheritHolder;
using reflection_fixture::InheritLeaf;
using reflection_fixture::InstancedHolder;
using reflection_fixture::InstancedHost;
using reflection_fixture::InstancedLocked;
using reflection_fixture::InstancedPoint;
using reflection_fixture::InstancedScale;

namespace {

// InstancedHolder 객체 Registry다(소멸: Registry → 등록 → 타입).
struct InstancedObjects {
    InstancedObjects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x4953, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~InstancedObjects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<InstancedHolder> Make() noexcept {
        auto created =
            objects.NewObject<InstancedHolder>(access, GeneratedType<InstancedHolder>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<InstancedHolder>{};
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
        auto borrowed = objects.Resolve<InstancedHolder>(access, object);
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
    TextDiagnostics diagnostics;
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

// 명령행 대입: 형 바꾸기 뒤 같은 묶음의 멤버 대입이 새 타입으로 풀리고(필드·Array 원소 끝 추가·
// Map 값·struct 멤버), `none`은 비운다. 형 바꾸기는 기본 값에서 시작한다.
TEST(TextInstanced_CommandLineSetsTypesAndMembers) {
    InstancedObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("value", "InstancedPoint"),
                                     Set("value.x", "5", 1),
                                     Set("value.label", "spot", 2),
                                     Set("list", "", 3),
                                     Set("list[0]", "InstancedScale", 4),
                                     Set("list[0].steps", "1,2", 5),
                                     Set("table[3]", "instancedpoint", 6),
                                     Set("table[3].y", "-4", 7),
                                     Set("host.value", "InstancedPoint", 8),
                                     Set("host.value.x", "9", 9),
                                     Set("spare", "none", 10)};
    REQUIRE(fixture.Apply(object, inputs));
    fixture.With(object, [](InstancedHolder& value) noexcept {
        const InstancedPoint* point = value.value.Get<InstancedPoint>();
        REQUIRE(point);
        CHECK(point->x == 5 && point->y == 0 && point->label.View() == "spot");
        REQUIRE(value.list.Size() == 1);
        const InstancedScale* scale = value.list[0].Get<InstancedScale>();
        REQUIRE(scale);
        CHECK(scale->factor == 1.0f && scale->steps.Size() == 2 && scale->steps[1] == 2u);
        REQUIRE(value.table.Find(3u));
        CHECK(value.table.Find(3u)->Get<InstancedPoint>() &&
              value.table.Find(3u)->Get<InstancedPoint>()->y == -4);
        CHECK(value.host.value.Get<InstancedPoint>() &&
              value.host.value.Get<InstancedPoint>()->x == 9);
        CHECK(value.spare.IsEmpty());
    });

    // 비우기와 형 바꾸기(같은 타입이어도 기본 값).
    const TextAssignment reset[] = {Set("value", "InstancedPoint"), Set("list[0]", "none", 1)};
    REQUIRE(fixture.Apply(object, reset));
    fixture.With(object, [](InstancedHolder& value) noexcept {
        CHECK(value.value.Get<InstancedPoint>() && value.value.Get<InstancedPoint>()->x == 0);
        CHECK(value.list.Size() == 1 && value.list[0].IsEmpty());
    });
}

// 거부는 진단이고 대상은 그대로다: 모르는 타입 도구 이름·클래스 이름(InvalidValue), ReadOnly 필드,
// 깊이 합 초과(host.value에 깊이 16 — InvalidValue), 비어 있는 값의 멤버(InvalidValue).
TEST(TextInstanced_RejectionsAreDiagnostics) {
    InstancedObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment cases[][1] = {{Set("value", "NoSuchStruct")},
                                       {Set("value", "InstancedHolder")},
                                       {Set("locked", "InstancedPoint")},
                                       {Set("host.value", "InstancedDepth16")},
                                       {Set("spare.x", "1")}};
    const TextDiagnosticCode codes[] = {
        TextDiagnosticCode::InvalidValue, TextDiagnosticCode::InvalidValue,
        TextDiagnosticCode::ReadOnly, TextDiagnosticCode::InvalidValue,
        TextDiagnosticCode::InvalidValue};
    for (usize index = 0; index < sizeof(codes) / sizeof(codes[0]); ++index) {
        auto applied = fixture.Apply(object, cases[index]);
        CHECK(!applied && applied.Error() == ErrorCode::InvalidArgument);
        REQUIRE(fixture.diagnostics.Count() >= 1);
        CHECK(fixture.diagnostics.Get(0).code == codes[index]);
    }

    fixture.With(object, [](InstancedHolder& value) noexcept {
        CHECK(value.value.IsEmpty() && value.locked.IsEmpty() && value.host.value.IsEmpty());
    });
}

// JSON: 비어 있음은 null, 값은 `{"_structType": "<타입 도구 이름>", 멤버...}`이고(원소·Map 값
// 포함), 그 JSON을 새 객체에 import하면 다시 쓴 텍스트가 같다. `{}`는 비우기이고 `_structType`가
// 없는 객체는 InvalidValue다.
TEST(TextInstanced_JsonRoundTripsStructType) {
    InstancedObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {
        Set("value", "InstancedPoint"),       Set("value.x", "5", 1),
        Set("list[0]", "InstancedScale", 2),  Set("table[3]", "InstancedPoint", 3),
        Set("fixed[1]", "InstancedPoint", 4), Set("fixed[1].y", "8", 5)};
    REQUIRE(fixture.Apply(object, inputs));
    StringBuilder<1024> text(fixture.system);
    REQUIRE(Export(AssignmentTarget::Live(fixture.objects, fixture.access, object), fixture.options,
                   text));
    // 필드 순서는 필드 ID 순이고 공백은 writer 규칙이라 토큰만 본다.
    CHECK(text.View().Contains("\"_structType\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"InstancedPoint\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"InstancedScale\"", StringComparison::CaseSensitive));
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
    StringBuilder<1024> again(fixture.system);
    REQUIRE(Export(AssignmentTarget::Live(fixture.objects, fixture.access, copy.Handle()),
                   fixture.options, again));
    CHECK(again.View() == text.View());

    JsonDocument cleared(fixture.system);
    REQUIRE(cleared.Parse("{\"value\":{}}").Succeeded());
    auto emptied =
        ImportJson(cleared.Root(), AssignmentTarget::Live(fixture.objects, fixture.access, object),
                   fixture.options, fixture.diagnostics);
    REQUIRE(emptied);
    REQUIRE(emptied.Value().Publish());
    fixture.With(object, [](InstancedHolder& value) noexcept { CHECK(value.value.IsEmpty()); });

    JsonDocument untyped(fixture.system);
    REQUIRE(untyped.Parse("{\"value\":{\"x\":1}}").Succeeded());
    fixture.diagnostics.Clear();
    auto rejected =
        ImportJson(untyped.Root(), AssignmentTarget::Live(fixture.objects, fixture.access, object),
                   fixture.options, fixture.diagnostics);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.diagnostics.Count() == 1 &&
          fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
}

// InstancedStruct<Base>(리플렉션 B9k — fixture StructInheritanceFixture.h): 파생 struct 이름은 받고
// 같은 묶음의 base·파생 멤버 대입이 그 위에서 풀린다. 무관한 struct 이름은 InvalidValue 진단이고
// 값은 그대로다.
TEST(TextInstanced_BaseRestrictsTypes) {
    InstancedObjects fixture;
    auto created = fixture.objects.NewObject<InheritHolder>(
        fixture.access, GeneratedType<InheritHolder>::Schema().id);
    REQUIRE(created);
    RootRef<InheritHolder> root = Move(created.Value());
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {Set("shape", "InheritLeaf"), Set("shape.a", "3", 1),
                                     Set("shape.c", "2.5", 2)};
    REQUIRE(fixture.Apply(object, inputs));
    const TextAssignment unrelated[] = {Set("shape", "InheritOther")};
    auto rejected = fixture.Apply(object, unrelated);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() >= 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
    auto borrowed = fixture.objects.Resolve<InheritHolder>(fixture.access, object);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    const InheritLeaf* leaf = value.Value()->shape.Get<InheritLeaf>();
    REQUIRE(leaf);
    CHECK(leaf->a == 3 && leaf->c == 2.5f);
}

// InstancedStruct 내용 안의 ReadOnly 멤버(B9j–B9m 묶음 리뷰 I3): 같은 값 대입은 무동작이고 다른
// 값은 ReadOnly 진단이며(진단 경로는 내용 멤버까지 — 리뷰 S2), 같은 객체의 JSON 왕복은 성공한다.
TEST(TextInstanced_ReadOnlyContentMembers) {
    InstancedObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment set[] = {Set("value", "InstancedLocked"), Set("value.free", "2", 1)};
    REQUIRE(fixture.Apply(object, set));
    fixture.With(object, [](InstancedHolder& value) noexcept {
        REQUIRE(value.value.Get<InstancedLocked>());
        value.value.Get<InstancedLocked>()->fixed = 3;
    });

    const TextAssignment same[] = {Set("value.fixed", "3")};
    REQUIRE(fixture.Apply(object, same));
    CHECK(fixture.diagnostics.Count() == 0);

    const TextAssignment different[] = {Set("value.fixed", "4")};
    auto rejected = fixture.Apply(object, different);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::ReadOnly);
    CHECK(fixture.diagnostics.Get(0).path == "value.fixed");

    StringBuilder<4096> text(fixture.system);
    REQUIRE(Export(AssignmentTarget::Live(fixture.objects, fixture.access, object), fixture.options,
                   text));
    JsonDocument document(fixture.system);
    REQUIRE(document.Parse(text.View()).Succeeded());
    fixture.diagnostics.Clear();
    auto imported =
        ImportJson(document.Root(), AssignmentTarget::Live(fixture.objects, fixture.access, object),
                   fixture.options, fixture.diagnostics);
    REQUIRE(imported);
    REQUIRE(imported.Value().Publish());

    JsonDocument changed(fixture.system);
    REQUIRE(
        changed.Parse("{\"value\":{\"_structType\":\"InstancedLocked\",\"fixed\":5,\"free\":2}}")
            .Succeeded());
    fixture.diagnostics.Clear();
    auto refused =
        ImportJson(changed.Root(), AssignmentTarget::Live(fixture.objects, fixture.access, object),
                   fixture.options, fixture.diagnostics);
    CHECK(!refused);
    REQUIRE(fixture.diagnostics.Count() >= 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::ReadOnly);
    CHECK(fixture.diagnostics.Get(0).path == "value.fixed");
    fixture.With(object, [](InstancedHolder& value) noexcept {
        CHECK(value.value.Get<InstancedLocked>() && value.value.Get<InstancedLocked>()->fixed == 3);
    });
}

// 대입 묶음 상자 안 형 바꾸기의 깊이 합은 원래 자리의 위치 깊이로 센다(리뷰 S1): struct 원소 배열
// 안의 InstancedStruct 멤버(위치 깊이 1)에 깊이 16 struct는 InvalidValue다.
TEST(TextInstanced_BoxDepthSumUsesOriginalPosition) {
    InstancedObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    fixture.With(object, [&](InstancedHolder& value) noexcept {
        REQUIRE(value.hosts.TryPushBack(InstancedHost{}));
    });

    const TextAssignment deep[] = {Set("hosts[0].value", "InstancedDepth16")};
    auto rejected = fixture.Apply(object, deep);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() >= 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);

    const TextAssignment fits[] = {Set("hosts[0].value", "InstancedDepth15")};
    REQUIRE(fixture.Apply(object, fits));
    fixture.With(object,
                 [](InstancedHolder& value) noexcept { CHECK(!value.hosts[0].value.IsEmpty()); });

    // 정적 배열 struct 원소 안 멤버는 값 단위가 원소 멤버라 상자에서 시작한다.
    const TextAssignment slotDeep[] = {Set("slots[1].value", "InstancedDepth16")};
    auto slotRejected = fixture.Apply(object, slotDeep);
    CHECK(!slotRejected && slotRejected.Error() == ErrorCode::InvalidArgument);
    REQUIRE(fixture.diagnostics.Count() >= 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
    const TextAssignment slotFits[] = {Set("slots[1].value", "InstancedDepth15")};
    REQUIRE(fixture.Apply(object, slotFits));
    fixture.With(object,
                 [](InstancedHolder& value) noexcept { CHECK(!value.slots[1].value.IsEmpty()); });
}

// 파생 struct 값 단위의 깊은 복사가 base의 NATIVE 멤버를 보존한다(리뷰 I1 — 대입 묶음
// Detach·Attach).
TEST(TextInstanced_DerivedUnitKeepsBaseNativeMembers) {
    InstancedObjects fixture;
    auto created = fixture.objects.NewObject<InheritHolder>(
        fixture.access, GeneratedType<InheritHolder>::Schema().id);
    REQUIRE(created);
    RootRef<InheritHolder> root = Move(created.Value());
    const ObjectHandle object = root.Handle();
    {
        auto borrowed = fixture.objects.Resolve<InheritHolder>(fixture.access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        value.Value()->leaf.nativeCount = 7;
        REQUIRE(value.Value()->leaf.note.TryAssign("kept"));
    }

    const TextAssignment inputs[] = {Set("leaf.c", "2.5"), Set("leaf.a", "4", 1)};
    REQUIRE(fixture.Apply(object, inputs));
    auto borrowed = fixture.objects.Resolve<InheritHolder>(fixture.access, object);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    CHECK(value.Value()->leaf.c == 2.5f && value.Value()->leaf.a == 4);
    CHECK(value.Value()->leaf.nativeCount == 7u);
    CHECK(value.Value()->leaf.note.View() == "kept");
}
