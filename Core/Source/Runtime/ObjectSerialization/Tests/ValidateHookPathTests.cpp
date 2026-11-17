#include <ReflectionTestFixtures/ValidateFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/DetachedPath.h>
#include <Object/PropertyPath.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// struct 검증 hook의 경로 쓰기 시험이다(Docs/Design/CurveData.ko.md 9.1절 — CV3, fixture
// ReflectionTestFixtures/ValidateFixture.h): 객체(게시·staged)의 변경 연산이 hook 있는 struct 안을
// 바꾸면 그 가장 바깥 hook struct를 담은 자리를 상자로 분리해 연산하고 상자 값 트리를 검증한 뒤
// 게시한다. 거부는 InvalidArgument(StructValidation)이고 객체는 그대로다. hook struct 안 자리에
// 직접 놓는 PathPrepareAttach는 Unsupported(InsideValidatedStruct)다. hook과 무관한 경로는 지금
// 그대로다.
using namespace rocks;
using reflection_fixture::CurrentValidateLog;
using reflection_fixture::ValidHolder;
using reflection_fixture::ValidRange;

namespace {

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

struct Objects {
    Objects() noexcept
        : types(system), objects(system, types, {0x5650, 1}), access(objects.OwnerAccess()) {
        REQUIRE(GeneratedType<ValidRange>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::ValidBox>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::ValidDerived>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::ValidRefElement>::Register(types));
        REQUIRE(GeneratedType<ValidHolder>::Register(types));
    }

    ~Objects() noexcept {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
    }

    RootRef<ValidHolder> Make() noexcept {
        auto created = objects.NewObject<ValidHolder>(access, TypeIdOf<ValidHolder>());
        if (!created) {
            platform::Fatal("test object creation failed");
        }

        return Move(created.Value());
    }

    template <class Edit>
    void With(ObjectHandle handle, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<ValidHolder>(access, handle);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    // 문자열 경로에 int32를 쓴다(실패 사유를 failure에 둔다).
    Result<void> Write(const PathTarget& target, ObjectHandle handle, const char* text, int32 value,
                       PropertyPathFailure* failure = nullptr) noexcept {
        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(objects, access, handle, StringView(text), path);
            !parsed) {
            return parsed;
        }

        FieldValue input(system);
        input.kind = ValueKind::Int32;
        input.signedInteger = value;
        return PathWrite(objects, access, target, path.View(), input, nullptr, failure);
    }

    SystemAllocator system;
    TypeRegistry types;
    ObjectRegistry objects;
    ObjectAccessContext access;
};

} // namespace

// 게시된 객체: 자리마다 유효한 쓰기는 게시되고, hook이 거부하는 쓰기는 InvalidArgument
// (StructValidation)이며 값이 그대로다. 중첩 struct는 안쪽 hook이 먼저다(box.range.high: r 다음 b).
TEST(ValidateHookPath_LiveWritesValidateEveryPosition) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PropertyPathBuffer element;
    REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle, "elements[0]", element));
    REQUIRE(PathInsertElement(fixture.objects, fixture.access, handle, element.View(), nullptr));
    fixture.With(handle, [&](ValidHolder& value) noexcept {
        REQUIRE(value.ranges.TryPushBack(ValidRange{}));
        value.maybe.Emplace(ValidRange{});
        REQUIRE(value.byKey.Insert(4, ValidRange{}));
        REQUIRE(value.any.Emplace<ValidRange>(fixture.types, fixture.system));
    });

    CurrentValidateLog().Clear();
    REQUIRE(fixture.Write(handle, handle, "box.range.high", 5));
    CHECK(CurrentValidateLog().Is("rb"));

    struct Case {
        const char* path;
        int32 valid;
        int32 invalid;
    };
    // low ≤ high(high는 0 — box.range.high는 위에서 5)를 깨는 쓰기와 지키는 쓰기다.
    const Case cases[] = {{"box.range.low", 4, 6},  {"box.limit", 5, 4},
                          {"ranges[0].low", -1, 1}, {"maybe.low", -2, 2},
                          {"byKey[4].low", -3, 3},  {"any.low", -4, 4},
                          {"fixed[1].low", -5, 5},  {"derived.low", -6, 6},
                          {"derived.extra", 7, -1}, {"elements[0].range.low", -8, 8}};
    for (const Case& item : cases) {
        PropertyPathFailure failure;
        CHECK(ErrorOf(fixture.Write(handle, handle, item.path, item.invalid, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::StructValidation);
        CHECK(fixture.Write(handle, handle, item.path, item.valid));
    }

    fixture.With(handle, [](ValidHolder& value) noexcept {
        CHECK(value.box.range.low == 4 && value.box.range.high == 5 && value.box.limit == 5);
        CHECK(value.ranges[0].low == -1 && value.maybe.Value().low == -2);
        CHECK(value.byKey.Find(4)->low == -3 && value.any.Get<ValidRange>()->low == -4);
        CHECK(value.fixed[1].low == -5 && value.derived.low == -6 && value.derived.extra == 7);
        CHECK(value.elements[0].range.low == -8);
    });

    // hook과 무관한 자리는 지금 경로 그대로다(hook을 부르지 않는다).
    CurrentValidateLog().Clear();
    REQUIRE(fixture.Write(handle, handle, "plain", 3));
    CHECK(CurrentValidateLog().count == 0);
}

// staged 객체도 같은 규칙이다(거부하면 그 객체 값은 그대로, 트랜잭션은 계속 쓸 수 있다).
TEST(ValidateHookPath_StagedWritesValidate) {
    Objects fixture;
    ConstructionContext transaction(fixture.objects, fixture.access);
    auto made = transaction.Create<ValidHolder>(TypeIdOf<ValidHolder>());
    REQUIRE(made);
    const PathTarget staged = PathTarget::Staged(transaction, made.Value());
    PropertyPathFailure failure;
    auto lease = fixture.types.Acquire(TypeIdOf<ValidHolder>());
    REQUIRE(lease);
    PropertyPathBuffer low;
    REQUIRE(ParsePropertyPath(lease.Value(), "box.range.low", low));
    FieldValue nine(fixture.system);
    nine.kind = ValueKind::Int32;
    nine.signedInteger = 9;
    CHECK(ErrorOf(PathWrite(fixture.objects, fixture.access, staged, low.View(), nine, nullptr,
                            &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::StructValidation);
    FieldValue read(fixture.system);
    REQUIRE(PathRead(fixture.objects, fixture.access, staged, low.View(), read));
    CHECK(read.signedInteger == 0);
    nine.signedInteger = -9;
    REQUIRE(PathWrite(fixture.objects, fixture.access, staged, low.View(), nine));
    REQUIRE(PathRead(fixture.objects, fixture.access, staged, low.View(), read));
    CHECK(read.signedInteger == -9);
}

// 상자를 객체에 놓을 때(PathPrepareAttach): 자리가 hook struct 안이면 Unsupported
// (InsideValidatedStruct)이고, 상자 값 트리가 hook에 걸리면 InvalidArgument(StructValidation)다.
TEST(ValidateHookPath_AttachChecksHooks) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PropertyPathBuffer range;
    REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle, "box.range", range));
    PropertyPathBuffer box;
    REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle, "box", box));
    PropertyPathBuffer boxLow;
    REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle, "box.range.low", boxLow));

    DetachedValue inner(fixture.objects.ObjectAllocator());
    REQUIRE(PathDetach(fixture.objects, fixture.access, handle, range.View(), inner));
    PreparedAttach prepared;
    PropertyPathFailure failure;
    CHECK(ErrorOf(PathPrepareAttach(fixture.objects, fixture.access, handle, range.View(), inner,
                                    prepared, &failure)) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::InsideValidatedStruct && failure.step == 0);

    DetachedValue outer(fixture.objects.ObjectAllocator());
    REQUIRE(PathDetach(fixture.objects, fixture.access, handle, box.View(), outer));
    FieldValue nine(fixture.system);
    nine.kind = ValueKind::Int32;
    nine.signedInteger = 9;
    // 상자 위 쓰기는 검증하지 않는다(상자는 사본 — 게시할 때 본다).
    REQUIRE(
        PathWrite(fixture.objects, fixture.access, PathTarget::Value(outer), boxLow.View(), nine));
    CHECK(!outer.ValidateTree());
    CHECK(ErrorOf(PathPrepareAttach(fixture.objects, fixture.access, handle, box.View(), outer,
                                    prepared, &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::StructValidation);
    nine.signedInteger = 0;
    REQUIRE(
        PathWrite(fixture.objects, fixture.access, PathTarget::Value(outer), boxLow.View(), nine));
    CHECK(outer.ValidateTree());
    REQUIRE(
        PathPrepareAttach(fixture.objects, fixture.access, handle, box.View(), outer, prepared));
}
