#include <ReflectionTestFixtures/ValidateFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/DetachedPath.h>
#include <Object/FieldEncodeContext.h>
#include <Object/PreparedStructElements.h>
#include <Object/PropertyPath.h>
#include <Platform/Fatal.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// struct 검증 hook의 경로 쓰기 시험이다(Docs/Design/CurveData/README.ko.md 9.1절 — CV3, fixture
// ReflectionTestFixtures/ValidateFixture.h): 객체(게시·staged)의 변경 연산이 hook 있는 struct 안을
// 바꾸면 그 가장 바깥 hook struct를 담은 자리를 상자로 분리해 연산하고 상자 값 트리를 검증한 뒤
// 게시한다. 거부는 InvalidArgument(StructValidation)이고 객체는 그대로다. hook struct 안 자리에
// 직접 놓는 PathPrepareAttach는 Unsupported(InsideValidatedStruct)다. hook과 무관한 경로는 지금
// 그대로다.
using namespace rocks;
namespace validate_hook_path_tests_file {

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
        REQUIRE(GeneratedType<reflection_fixture::ValidNonEmpty>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::ValidWrapper>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::ValidList>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::ValidOuter>::Register(types));
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

namespace {

PropertyPathBuffer PathOf(Objects& fixture, ObjectHandle handle, const char* text) noexcept {
    PropertyPathBuffer path;
    CHECK(ParsePropertyPath(fixture.objects, fixture.access, handle, StringView(text), path));
    return path;
}

FieldValue Int32Value(Allocator& allocator, int32 value) noexcept {
    FieldValue input(allocator);
    input.kind = ValueKind::Int32;
    input.signedInteger = value;
    return input;
}

// encode 경로 없이 staged 트랜잭션에 Required 검사 중 표시를 세우는 최소 문맥이다(시험 전용).
class RequiredCheckMarker final : public FieldEncodeContext {
public:
    RequiredCheckMarker(const FieldEncodePolicy& policy, ConstructionContext& transaction,
                        Allocator& allocator) noexcept
        : FieldEncodeContext(policy, nullptr, static_cast<uint32>(WireType::U32)),
          allocator_(allocator) {
        BeginRequiredCheck(transaction);
    }

    RecordLimits NestedRecordLimits() const noexcept override {
        return {};
    }

    Result<void> WriteReferenceWire(ArchiveWriter&, ObjectHandle,
                                    const ReferenceExpectation&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    Result<void> Spend(uint64) noexcept override {
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<void> AddElements(usize) noexcept override {
        return Result<void>::Success();
    }

private:
    Allocator& allocator_;
};

// 할당마다(켜 있으면) 콜백을 부르는 allocator다(재진입 변경 시험).
class CallbackAllocator final : public Allocator {
public:
    explicit CallbackAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (armed && callback) {
            armed = false;
            callback(state);
            armed = true;
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    void (*callback)(void*) noexcept = nullptr;
    void* state = nullptr;
    bool armed = false;

private:
    Allocator& inner_;
};

} // namespace

// PathPrepareAttach는 자리 바로 위 InstancedStruct에 담긴 hook struct도 본다(CV3 리뷰 2): any에
// ValidBox를 담고 any.range를 분리해 range.high를 limit 위로 바꾼 상자는 range 자신은 통과해도 놓을
// 수 없다.
TEST(ValidateHookPath_AttachSeesHookInsideInstancedStruct) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PropertyPathBuffer any = PathOf(fixture, handle, "any");
    REQUIRE(PathSetInstancedType(fixture.objects, fixture.access, handle, any.View(),
                                 TypeIdOf<reflection_fixture::ValidBox>()));
    PropertyPathBuffer range = PathOf(fixture, handle, "any.range");
    PropertyPathBuffer high = PathOf(fixture, handle, "any.range.high");
    DetachedValue box(fixture.objects.ObjectAllocator());
    REQUIRE(PathDetach(fixture.objects, fixture.access, handle, range.View(), box));
    FieldValue large = Int32Value(fixture.system, 500);
    const PropertyPath boxHigh{
        Span<const PathStep>(high.View().steps.Data() + 1, high.View().steps.Size() - 1), {}};
    REQUIRE(PathWrite(fixture.objects, fixture.access, PathTarget::Value(box), boxHigh, large));
    CHECK(box.ValidateTree());
    PreparedAttach prepared;
    PropertyPathFailure failure;
    CHECK(ErrorOf(PathPrepareAttach(fixture.objects, fixture.access, handle, range.View(), box,
                                    prepared, &failure)) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::InsideValidatedStruct && failure.step == 0);
}

// 값을 만드는 연산은 만든 기본값의 hook 트리를 본다(CV3 리뷰 5): 기본값이 어기는 ValidNonEmpty를
// 형 바꾸기·원소 넣기·Optional 만들기·키 넣기로 만들면 거부되고 그대로다.
TEST(ValidateHookPath_CreatedDefaultsAreValidated) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PropertyPathFailure failure;
    PropertyPathBuffer any = PathOf(fixture, handle, "any");
    CHECK(ErrorOf(PathSetInstancedType(fixture.objects, fixture.access, handle, any.View(),
                                       TypeIdOf<reflection_fixture::ValidNonEmpty>(), &failure)) ==
          ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::StructValidation);
    PropertyPathBuffer wrapper = PathOf(fixture, handle, "wrappers[0]");
    CHECK(ErrorOf(PathInsertElement(fixture.objects, fixture.access, handle, wrapper.View(),
                                    nullptr)) == ErrorCode::InvalidArgument);
    PropertyPathBuffer maybe = PathOf(fixture, handle, "maybeEmpty");
    CHECK(ErrorOf(PathEmplaceOptional(fixture.objects, fixture.access, handle, maybe.View())) ==
          ErrorCode::InvalidArgument);
    PropertyPathBuffer key = PathOf(fixture, handle, "byKeyEmpty[3]");
    CHECK(!PathInsertKey(fixture.objects, fixture.access, handle, key.View()));
    fixture.With(handle, [](ValidHolder& value) noexcept {
        CHECK(value.any.IsEmpty() && value.wrappers.Size() == 0 && !value.maybeEmpty.HasValue() &&
              value.byKeyEmpty.Count() == 0);
    });

    // 기본값이 맞는 struct는 그대로 만든다.
    REQUIRE(PathSetInstancedType(fixture.objects, fixture.access, handle, any.View(),
                                 TypeIdOf<ValidRange>()));
}

// hook struct 안의 모든 변경 연산이 상자 경로를 거친다(CV3 리뷰 12): 원소 넣기·지우기·비우기, 키
// 넣기·지우기, Optional 만들기·비우기, 형 바꾸기·비우기. 바뀐 것이 없는 키 넣기는 게시하지
// 않고(변경 표지 그대로 — 리뷰 7), 상자 위 실패 단계는 원래 경로 단계로 옮기며, 거부된 쓰기의 출력
// 인자는 그대로다(리뷰 6).
TEST(ValidateHookPath_EveryOperationInsideHook) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PropertyPathBuffer front = PathOf(fixture, handle, "outer.list.values[0]");
    for (int32 index = 0; index < 3; ++index) {
        FieldValue input = Int32Value(fixture.system, index);
        REQUIRE(PathInsertElement(fixture.objects, fixture.access, handle, front.View(), &input));
    }

    PropertyPathFailure failure;
    PropertyPathBuffer fourth = PathOf(fixture, handle, "outer.list.values[3]");
    FieldValue nine = Int32Value(fixture.system, 9);
    CHECK(ErrorOf(PathInsertElement(fixture.objects, fixture.access, handle, fourth.View(), &nine,
                                    &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::StructValidation);

    // 상자 위 연산의 실패 단계: outer(0).list(1).values[9](2) — hook(list)은 1단계라 상자 경로 1 +
    // 1이다.
    PropertyPathBuffer far = PathOf(fixture, handle, "outer.list.values[9]");
    CHECK(ErrorOf(PathInsertElement(fixture.objects, fixture.access, handle, far.View(), &nine,
                                    &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::IndexOutOfRange && failure.step == 2);

    REQUIRE(PathRemoveElement(fixture.objects, fixture.access, handle, front.View()));
    PropertyPathBuffer values = PathOf(fixture, handle, "outer.list.values");
    REQUIRE(PathClear(fixture.objects, fixture.access, handle, values.View()));

    PropertyPathBuffer five = PathOf(fixture, handle, "outer.list.map[5]");
    PropertyPathBuffer six = PathOf(fixture, handle, "outer.list.map[6]");
    PropertyPathBuffer seven = PathOf(fixture, handle, "outer.list.map[7]");
    auto inserted = PathInsertKey(fixture.objects, fixture.access, handle, five.View());
    REQUIRE(inserted);
    CHECK(inserted.Value());
    auto stamp = fixture.objects.MutationStamp(fixture.access, handle);
    REQUIRE(stamp);
    auto again = PathInsertKey(fixture.objects, fixture.access, handle, five.View());
    REQUIRE(again);
    CHECK(!again.Value());
    auto unchanged = fixture.objects.MutationStamp(fixture.access, handle);
    REQUIRE(unchanged);
    CHECK(unchanged.Value() == stamp.Value());
    REQUIRE(PathInsertKey(fixture.objects, fixture.access, handle, six.View()));
    auto third = PathInsertKey(fixture.objects, fixture.access, handle, seven.View(), &failure);
    CHECK(!third && third.Error() == ErrorCode::InvalidArgument);
    REQUIRE(PathEraseKey(fixture.objects, fixture.access, handle, six.View()));

    PropertyPathBuffer opt = PathOf(fixture, handle, "outer.list.opt");
    REQUIRE(PathEmplaceOptional(fixture.objects, fixture.access, handle, opt.View()));
    REQUIRE(PathResetOptional(fixture.objects, fixture.access, handle, opt.View()));
    PropertyPathBuffer inner = PathOf(fixture, handle, "outer.list.inner");
    CHECK(ErrorOf(PathSetInstancedType(fixture.objects, fixture.access, handle, inner.View(),
                                       TypeIdOf<reflection_fixture::ValidNonEmpty>())) ==
          ErrorCode::InvalidArgument);
    REQUIRE(PathSetInstancedType(fixture.objects, fixture.access, handle, inner.View(),
                                 TypeIdOf<ValidRange>()));
    REQUIRE(PathClearInstanced(fixture.objects, fixture.access, handle, inner.View()));
    fixture.With(handle, [](ValidHolder& value) noexcept {
        CHECK(value.outer.list.values.Size() == 0 && value.outer.list.map.Count() == 1 &&
              !value.outer.list.opt.HasValue() && value.outer.list.inner.IsEmpty());
    });

    // 거부된 쓰기는 출력 인자(previous)를 바꾸지 않는다.
    PropertyPathBuffer high = PathOf(fixture, handle, "box.range.high");
    FieldValue large = Int32Value(fixture.system, 500);
    FieldValue previous(fixture.system);
    previous.kind = ValueKind::Bool;
    previous.boolean = true;
    CHECK(ErrorOf(PathWrite(fixture.objects, fixture.access, handle, high.View(), large,
                            &previous)) == ErrorCode::InvalidArgument);
    CHECK(previous.kind == ValueKind::Bool && previous.boolean);
}

// Busy 규칙(CV3 리뷰 4·8): 상자 경로도 제자리 경로와 같은 판정이다 — encode 고정 중이면 Busy,
// staged Required 검사 중 표시 동안 Busy.
TEST(ValidateHookPath_BusyRulesMatchInPlaceWrites) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PropertyPathBuffer high = PathOf(fixture, handle, "box.range.high");
    PropertyPathBuffer plain = PathOf(fixture, handle, "plain");
    FieldValue five = Int32Value(fixture.system, 5);
    {
        auto pin = fixture.objects.PinForEncode(fixture.access, handle);
        REQUIRE(pin);
        CHECK(ErrorOf(PathWrite(fixture.objects, fixture.access, handle, plain.View(), five)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(fixture.objects, fixture.access, handle, high.View(), five)) ==
              ErrorCode::Busy);
    }

    CHECK(PathWrite(fixture.objects, fixture.access, handle, high.View(), five));

    ConstructionContext transaction(fixture.objects, fixture.access);
    auto made = transaction.Create<ValidHolder>(TypeIdOf<ValidHolder>());
    REQUIRE(made);
    const PathTarget staged = PathTarget::Staged(transaction, made.Value());
    FieldEncodePolicy policy;
    {
        RequiredCheckMarker marker(policy, transaction, fixture.system);
        CHECK(ErrorOf(PathWrite(fixture.objects, fixture.access, staged, plain.View(), five)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(fixture.objects, fixture.access, staged, high.View(), five)) ==
              ErrorCode::Busy);
    }

    CHECK(PathWrite(fixture.objects, fixture.access, staged, high.View(), five));
}

// 분리와 게시 사이 재진입 변경(CV3 리뷰 11): 상자 연산의 할당 콜백이 같은 트랜잭션에 staged 쓰기를
// 하면 게시하지 않고 Busy다(그 쓰기를 덮어쓰지 않는다).
TEST(ValidateHookPath_ReentrantStagedWriteIsBusy) {
    SystemAllocator system;
    CallbackAllocator allocator(system);
    TypeRegistry types(system);
    REQUIRE(GeneratedType<ValidRange>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidBox>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidDerived>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidRefElement>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidNonEmpty>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidWrapper>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidList>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidOuter>::Register(types));
    REQUIRE(GeneratedType<ValidHolder>::Register(types));
    {
        ObjectRegistry objects(allocator, types, {0x5652, 1});
        const ObjectAccessContext access = objects.OwnerAccess();
        {
            ConstructionContext transaction(objects, access);
            auto made = transaction.Create<ValidHolder>(TypeIdOf<ValidHolder>());
            REQUIRE(made);
            struct State {
                ConstructionContext* transaction;
                ObjectHandle object;
            } state{&transaction, made.Value()};
            auto lease = types.Acquire(TypeIdOf<ValidHolder>());
            REQUIRE(lease);
            PropertyPathBuffer at;
            REQUIRE(ParsePropertyPath(lease.Value(), "outer.list.values[0]", at));
            FieldValue one(system);
            one.kind = ValueKind::Int32;
            one.signedInteger = 1;
            allocator.state = &state;
            allocator.callback = [](void* raw) noexcept {
                auto* reentry = static_cast<State*>(raw);
                (void)reentry->transaction->GetForWrite<Object>(reentry->object);
            };
            allocator.armed = true;
            auto inserted = PathInsertElement(
                objects, access, PathTarget::Staged(transaction, made.Value()), at.View(), &one);
            allocator.armed = false;
            CHECK(ErrorOf(inserted) == ErrorCode::Busy);
            auto value = transaction.Get<ValidHolder>(made.Value());
            REQUIRE(value);
            CHECK(value.Value()->outer.list.values.Size() == 0);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 이미 있는 Map 키의 InsertKey는 값을 기본 값으로 바꾸는 변경이다(CV4 리뷰 1): 상자 경로가 길이 0을
// "바뀐 것 없음"으로 보던 것은 Set 원소에만 맞다. Map<K, hook struct>의 기존 키 초기화는
// 게시되고(변경 표지가 오른다), 기본 값이 hook을 어기면 InvalidArgument(StructValidation)이고 값이
// 그대로다. Set의 이미 있는 원소는 지금처럼 게시하지 않는다(EveryOperationInsideHook).
TEST(ValidateHookPath_InsertKeyResetsExistingMapValue) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    fixture.With(handle, [](ValidHolder& value) noexcept {
        REQUIRE(value.byKey.Insert(int32{4}, ValidRange{-5, 3}));
        REQUIRE(value.byKeyEmpty.Insert(int32{2}, reflection_fixture::ValidNonEmpty{7}));
    });

    PropertyPathBuffer four = PathOf(fixture, handle, "byKey[4]");
    auto stamp = fixture.objects.MutationStamp(fixture.access, handle);
    REQUIRE(stamp);
    auto reset = PathInsertKey(fixture.objects, fixture.access, handle, four.View());
    REQUIRE(reset);
    CHECK(!reset.Value());
    auto after = fixture.objects.MutationStamp(fixture.access, handle);
    REQUIRE(after);
    CHECK(after.Value() != stamp.Value());

    PropertyPathBuffer two = PathOf(fixture, handle, "byKeyEmpty[2]");
    PropertyPathFailure failure;
    auto rejected = PathInsertKey(fixture.objects, fixture.access, handle, two.View(), &failure);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::StructValidation);
    fixture.With(handle, [](ValidHolder& value) noexcept {
        REQUIRE(value.byKey.Find(4) != nullptr && value.byKeyEmpty.Find(2) != nullptr);
        CHECK(value.byKey.Find(4)->low == 0 && value.byKey.Find(4)->high == 0);
        CHECK(value.byKeyEmpty.Find(2)->count == 7);
    });
}

// staged 재진입 감지는 분리(PathDetach) 전부터 잰다(CV4 리뷰 3): 분리의 할당 콜백이 같은 트랜잭션에
// 쓰고 상자 연산이 할당하지 않는 쓰기(ranges[0].low)이면, 분리 뒤에 잰 누계로는 놓쳐 상자 맞바꿈이
// 그 쓰기를 덮을 수 있었다. 지금은 Busy이고 값은 그대로다.
TEST(ValidateHookPath_ReentryDuringDetachIsBusy) {
    SystemAllocator system;
    CallbackAllocator allocator(system);
    TypeRegistry types(system);
    REQUIRE(GeneratedType<ValidRange>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidBox>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidDerived>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidRefElement>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidNonEmpty>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidWrapper>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidList>::Register(types));
    REQUIRE(GeneratedType<reflection_fixture::ValidOuter>::Register(types));
    REQUIRE(GeneratedType<ValidHolder>::Register(types));
    {
        ObjectRegistry objects(allocator, types, {0x5653, 1});
        const ObjectAccessContext access = objects.OwnerAccess();
        {
            ConstructionContext transaction(objects, access);
            auto made = transaction.Create<ValidHolder>(TypeIdOf<ValidHolder>());
            REQUIRE(made);
            struct State {
                ConstructionContext* transaction;
                ObjectHandle object;
                bool fired;
            } state{&transaction, made.Value(), false};
            auto lease = types.Acquire(TypeIdOf<ValidHolder>());
            REQUIRE(lease);
            PropertyPathBuffer at;
            // 분리(배열 원소 버퍼 사본)가 할당하도록 원소 하나를 미리 둔다(상자 저장소 자체는
            // 인라인일 수 있다).
            auto staged = transaction.GetForWrite<ValidHolder>(made.Value());
            REQUIRE(staged);
            REQUIRE(staged.Value()->ranges.TryPushBack(ValidRange{}));
            REQUIRE(ParsePropertyPath(lease.Value(), "ranges[0].low", at));
            FieldValue minusOne(system);
            minusOne.kind = ValueKind::Int32;
            minusOne.signedInteger = -1;
            allocator.state = &state;
            // 첫 할당(상자 분리)에서 한 번만 같은 트랜잭션에 쓴다.
            allocator.callback = [](void* raw) noexcept {
                auto* reentry = static_cast<State*>(raw);
                if (reentry->fired) {
                    return;
                }

                reentry->fired = true;
                auto written = reentry->transaction->GetForWrite<ValidHolder>(reentry->object);
                if (written) {
                    written.Value()->plain = 77;
                }
            };
            allocator.armed = true;
            auto result = PathWrite(objects, access, PathTarget::Staged(transaction, made.Value()),
                                    at.View(), minusOne);
            allocator.armed = false;
            CHECK(state.fired);
            CHECK(ErrorOf(result) == ErrorCode::Busy);
            auto value = transaction.Get<ValidHolder>(made.Value());
            REQUIRE(value);
            CHECK(value.Value()->ranges[0].low == 0 && value.Value()->plain == 77);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }
}

// PreparedStructElements를 대입 묶음 없이 직접 써도 추가 원소는 게시 전에 검증된다(CV3 리뷰 3):
// PreparePublish가 hook 거부로 InvalidArgument이고, 묶지 않은 준비물의 ValidateAdded도
// InvalidArgument다.
TEST(ValidateHookPath_PreparedElementsValidateAddedDirectly) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PreparedStructElements unbound(fixture.system);
    usize rejected = 0;
    CHECK(ErrorOf(unbound.ValidateAdded(rejected)) == ErrorCode::InvalidArgument);

    PropertyPathBuffer elements = PathOf(fixture, handle, "elements");
    PreparedStructElements stage(fixture.system);
    REQUIRE(
        PathPrepareStructElements(fixture.objects, fixture.access, handle, elements.View(), stage));
    auto id = stage.Append();
    REQUIRE(id);
    auto start = stage.AddedTarget(id.Value());
    REQUIRE(start);
    auto lease = fixture.types.AcquireValueType(TypeIdOf<reflection_fixture::ValidRefElement>(),
                                                TypeKind::Struct);
    REQUIRE(lease);
    PropertyPathBuffer low;
    REQUIRE(ParsePropertyPath(lease.Value(), "range.low", low));
    FieldValue nine = Int32Value(fixture.system, 9);
    REQUIRE(PathWrite(fixture.objects, fixture.access, start.Value(), low.View(), nine));
    CHECK(ErrorOf(stage.PreparePublish()) == ErrorCode::InvalidArgument);
}

// ValidateAdded 뒤에 AddedTarget 시작 위치로 쓴 변경도 게시 전에 다시 검증된다(CV4 리뷰 4): 블록을
// 바꾸는 경로 연산이 검증 표시를 내린다. 읽기는 내리지 않는다(다시 부르지 않아도 게시된다).
TEST(ValidateHookPath_PreparedElementsWriteAfterValidateIsRevalidated) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    PropertyPathBuffer elements = PathOf(fixture, handle, "elements");
    PreparedStructElements stage(fixture.system);
    REQUIRE(
        PathPrepareStructElements(fixture.objects, fixture.access, handle, elements.View(), stage));
    auto id = stage.Append();
    REQUIRE(id);
    auto start = stage.AddedTarget(id.Value());
    REQUIRE(start);
    usize rejected = 0;
    REQUIRE(stage.ValidateAdded(rejected));
    auto lease = fixture.types.AcquireValueType(TypeIdOf<reflection_fixture::ValidRefElement>(),
                                                TypeKind::Struct);
    REQUIRE(lease);
    PropertyPathBuffer low;
    REQUIRE(ParsePropertyPath(lease.Value(), "range.low", low));
    FieldValue nine = Int32Value(fixture.system, 9);
    REQUIRE(PathWrite(fixture.objects, fixture.access, start.Value(), low.View(), nine));
    CHECK(ErrorOf(stage.PreparePublish()) == ErrorCode::InvalidArgument);
}
} // namespace validate_hook_path_tests_file
