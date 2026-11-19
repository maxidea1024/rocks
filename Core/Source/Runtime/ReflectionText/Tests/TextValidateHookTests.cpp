#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/ValidateFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextJson.h>
#include <TestSupport/Test.h>

// struct 검증 hook의 대입 묶음·JSON import 시험이다(Docs/Design/CurveData.ko.md 9.1절 — CV3,
// fixture ReflectionTestFixtures/ValidateFixture.h): 준비한 사본(상자)에 모든 대입을 적용한 뒤
// Publish 전에 hook을 부른다. 묶음 안에서 잠시 깨졌다가 끝에 맞는 대입은 통과하고, 끝 값이 거부되면
// 진단 InvalidStructValue와 InvalidArgument로 아무것도 게시하지 않는다(다른 단위도). StructRefArray
// 추가 원소의 hook 멤버도 본다. 같은 값의 무동작 대입은 hook을 부르지 않는다.
using namespace rocks;
namespace text_validate_hook_tests_file {

using namespace rocks::reflection_text;
using reflection_fixture::CurrentValidateLog;
using reflection_fixture::ValidHolder;
using reflection_fixture::ValidRange;

namespace {

TextAssignment Set(const char* path, const char* value, uint32 source = 0) noexcept {
    TextAssignment assignment;
    assignment.kind = AssignmentKind::Set;
    assignment.path = StringView(path);
    assignment.value = StringView(value);
    assignment.sourceIndex = source;
    return assignment;
}

struct Objects {
    Objects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x5651, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~Objects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<ValidHolder> Make() noexcept {
        auto created =
            objects.NewObject<ValidHolder>(access, GeneratedType<ValidHolder>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<ValidHolder>{};
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

    Result<void> Import(ObjectHandle object, StringView json) noexcept {
        diagnostics.Clear();
        JsonDocument document(system);
        if (!document.Parse(json).Succeeded()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto imported = ImportJson(document.Root(), AssignmentTarget::Live(objects, access, object),
                                   options, diagnostics);
        if (!imported) {
            return Result<void>::Failure(imported.Error());
        }

        auto published = imported.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

    template <class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<ValidHolder>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    bool OnlyStructDiagnostic(StringView path) const noexcept {
        return diagnostics.Count() == 1 &&
               diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue &&
               diagnostics.Get(0).path == path;
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    reflection_text::TextDiagnostics diagnostics;
    TextCodecOptions options;
};

} // namespace

// 묶음 안에서 순서상 잠시 깨지는 대입(low를 먼저 올림)도 끝 값이 맞으면 게시한다. 끝 값이
// 거부되면 InvalidStructValue(단위 경로 — 그 struct를 담은 필드)이고 같은 묶음의 다른 단위도
// 게시하지 않는다.
TEST(TextValidateHook_BatchValidatesFinalValues) {
    Objects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment valid[] = {Set("box.range.low", "5"), Set("box.range.high", "8", 1),
                                    Set("plain", "3", 2)};
    CurrentValidateLog().Clear();
    REQUIRE(fixture.Apply(object, valid));
    CHECK(CurrentValidateLog().Is("rb"));
    fixture.With(object, [](ValidHolder& value) noexcept {
        CHECK(value.box.range.low == 5 && value.box.range.high == 8 && value.plain == 3);
    });

    const TextAssignment invalid[] = {Set("plain", "9"), Set("box.range.low", "9", 1)};
    CHECK(!fixture.Apply(object, invalid));
    CHECK(fixture.OnlyStructDiagnostic("box"));
    fixture.With(object, [](ValidHolder& value) noexcept {
        CHECK(value.box.range.low == 5 && value.plain == 3);
    });

    // 바깥 hook(range.high ≤ limit)의 거부도 같다.
    const TextAssignment limit[] = {Set("box.limit", "7")};
    CHECK(!fixture.Apply(object, limit));
    CHECK(fixture.OnlyStructDiagnostic("box"));

    // 원소·Optional·정적 배열·파생 struct 자리.
    const TextAssignment ranges[] = {Set("ranges[0]", "{}"), Set("ranges[0].low", "3", 1)};
    CHECK(!fixture.Apply(object, ranges));
    CHECK(fixture.OnlyStructDiagnostic("ranges"));
    const TextAssignment maybe[] = {Set("maybe", "{}"), Set("maybe.low", "3", 1)};
    CHECK(!fixture.Apply(object, maybe));
    CHECK(fixture.OnlyStructDiagnostic("maybe"));
    const TextAssignment fixed[] = {Set("fixed[1].low", "1")};
    CHECK(!fixture.Apply(object, fixed));
    CHECK(fixture.OnlyStructDiagnostic("fixed"));
    const TextAssignment derived[] = {Set("derived.extra", "-1")};
    CHECK(!fixture.Apply(object, derived));
    CHECK(fixture.OnlyStructDiagnostic("derived"));

    // 같은 값의 무동작 대입은 hook을 부르지 않는다.
    const TextAssignment same[] = {Set("box.range.low", "5")};
    CurrentValidateLog().Clear();
    REQUIRE(fixture.Apply(object, same));
    CHECK(CurrentValidateLog().count == 0);
}

// StructRefArray 추가 원소의 hook 멤버(블록에 바로 적용된다)도 게시 전에 본다.
TEST(TextValidateHook_AddedElementIsValidated) {
    Objects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment invalid[] = {Set("elements[0]", "{}"),
                                      Set("elements[0].range.low", "4", 1)};
    CHECK(!fixture.Apply(object, invalid));
    CHECK(fixture.OnlyStructDiagnostic("elements[0]"));
    fixture.With(object, [](ValidHolder& value) noexcept { CHECK(value.elements.Size() == 0); });

    const TextAssignment valid[] = {Set("elements[0]", "{}"),
                                    Set("elements[0].range.high", "4", 1)};
    REQUIRE(fixture.Apply(object, valid));
    fixture.With(object, [](ValidHolder& value) noexcept {
        REQUIRE(value.elements.Size() == 1);
        CHECK(value.elements[0].range.high == 4);
    });
}

// JSON import도 같은 묶음이다: 값 트리 안의 모든 hook 인스턴스(Map 값·InstancedStruct 내용 포함)를
// 게시 전에 보고, 거부하면 아무 필드도 바뀌지 않는다.
TEST(TextValidateHook_JsonImportValidates) {
    Objects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    REQUIRE(
        fixture.Import(object, "{\"plain\": 4, \"byKey\": {\"1\": {\"low\": 0, \"high\": 1}}}"));
    fixture.With(object, [](ValidHolder& value) noexcept {
        CHECK(value.plain == 4);
        REQUIRE(value.byKey.Find(1));
    });

    CHECK(!fixture.Import(object, "{\"plain\": 5, \"byKey\": {\"2\": {\"low\": 3, \"high\": 1}}}"));
    CHECK(fixture.OnlyStructDiagnostic("byKey"));
    fixture.With(object, [](ValidHolder& value) noexcept {
        CHECK(value.plain == 4 && !value.byKey.Find(2));
    });
}

// struct 값 대상(AssignmentTarget::Value — 설정·콘솔 경로)과 staged 대상도 게시 전에 hook을
// 부른다(CV3 리뷰 1): 멤버 hook(range)은 단위 상자 검증으로, 루트 자신의 hook(ValidBox: range.high
// ≤ limit)은 루트 사본 검증으로 거부하고 값은 그대로다.
TEST(TextValidateHook_ValueAndStagedTargets) {
    Objects fixture;
    auto lease = fixture.types.AcquireValueType(
        GeneratedType<reflection_fixture::ValidBox>::Schema().id, TypeKind::Struct);
    REQUIRE(lease);
    reflection_fixture::ValidBox value;
    const auto apply = [&](Span<const TextAssignment> inputs) noexcept -> Result<void> {
        fixture.diagnostics.Clear();
        auto prepared =
            PrepareAssignments(AssignmentTarget::Value(fixture.objects, fixture.access,
                                                       lease.Value(), &value, fixture.system),
                               inputs, fixture.options, fixture.diagnostics);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        auto published = prepared.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    };

    const TextAssignment member[] = {Set("range.low", "9")};
    CHECK(!apply(member));
    CHECK(fixture.OnlyStructDiagnostic("range"));
    const TextAssignment root[] = {Set("limit", "-1")};
    CHECK(!apply(root));
    CHECK(fixture.OnlyStructDiagnostic(""));
    CHECK(value.range.low == 0 && value.limit == 100);
    const TextAssignment fine[] = {Set("range.high", "7"), Set("limit", "8", 1)};
    REQUIRE(apply(fine));
    CHECK(value.range.high == 7 && value.limit == 8);

    ConstructionContext transaction(fixture.objects, fixture.access);
    auto made = transaction.Create<ValidHolder>(GeneratedType<ValidHolder>::Schema().id);
    REQUIRE(made);
    const TextAssignment staged[] = {Set("box.range.low", "9")};
    fixture.diagnostics.Clear();
    auto prepared = PrepareAssignments(AssignmentTarget::Staged(transaction, made.Value()), staged,
                                       fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    CHECK(!prepared.Value().Publish());
    CHECK(fixture.OnlyStructDiagnostic("box"));
}
} // namespace text_validate_hook_tests_file
