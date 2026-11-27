#include <ReflectionTestFixtures/ValidateFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// struct 검증 hook의 graph decode 시험이다(Docs/Design/CurveData/README.ko.md 9.1절 — CV3, fixture
// ReflectionTestFixtures/ValidateFixture.h): 복원 2단계가 struct 값을 다 채운 뒤 게시 전에 hook을
// 부른다. 모든 자리(struct 필드·멤버·Array·정적 배열 원소·Optional 내부·Map 값·InstancedStruct
// 내용· StructRefArray 원소 멤버·base hook)에서 거부하면 복원 전체가 InvalidArgument이고 대상
// Registry에 아무것도 게시되지 않는다. 중첩은 안쪽 hook이 먼저다.
using namespace rocks;
namespace validate_hook_graph_tests_file {

using reflection_fixture::CurrentValidateLog;
using reflection_fixture::ValidDerived;
using reflection_fixture::ValidHolder;
using reflection_fixture::ValidRange;

namespace {

bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<ValidRange>::Register(types).IsOk() &&
           GeneratedType<reflection_fixture::ValidBox>::Register(types).IsOk() &&
           GeneratedType<ValidDerived>::Register(types).IsOk() &&
           GeneratedType<reflection_fixture::ValidRefElement>::Register(types).IsOk() &&
           GeneratedType<reflection_fixture::ValidNonEmpty>::Register(types).IsOk() &&
           GeneratedType<reflection_fixture::ValidWrapper>::Register(types).IsOk() &&
           GeneratedType<reflection_fixture::ValidList>::Register(types).IsOk() &&
           GeneratedType<reflection_fixture::ValidOuter>::Register(types).IsOk() &&
           GeneratedType<ValidHolder>::Register(types).IsOk();
}

RootRef<ValidHolder> New(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<ValidHolder>(objects.OwnerAccess(), TypeIdOf<ValidHolder>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

template <class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<ValidHolder>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

ValidRange Range(int32 low, int32 high) noexcept {
    ValidRange range;
    range.low = low;
    range.high = high;
    return range;
}

// 모든 자리를 유효한 값으로 채운다(StructRefArray 원소는 경로 연산으로 넣는다).
void FillValid(TypeRegistry& types, ObjectRegistry& objects, ObjectHandle handle,
               Allocator& allocator) noexcept {
    PropertyPathBuffer element;
    REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "elements[0]", element));
    REQUIRE(PathInsertElement(objects, objects.OwnerAccess(), handle, element.View(), nullptr));
    With(objects, handle, [&](ValidHolder& value) noexcept {
        value.box.range = Range(1, 2);
        value.box.limit = 10;
        REQUIRE(value.ranges.TryPushBack(Range(0, 1)));
        REQUIRE(value.ranges.TryPushBack(Range(3, 4)));
        value.maybe.Emplace(Range(5, 6));
        REQUIRE(value.byKey.Insert(7, Range(7, 8)));
        REQUIRE(value.any.Emplace<ValidRange>(types, allocator));
        *value.any.Get<ValidRange>() = Range(9, 10);
        value.fixed[0] = Range(1, 1);
        value.fixed[1] = Range(2, 3);
        value.derived.low = 1;
        value.derived.high = 2;
        value.derived.extra = 3;
        value.elements[0].range = Range(4, 5);
        value.plain = 11;
    });
}

// 한 자리를 잘못된 값으로 바꾼 holder를 저장하고 새 Registry에 복원한다(복원 결과의 오류).
template <class Edit>
ErrorCode LoadWith(Allocator& allocator, TypeRegistry& types, Edit&& edit) noexcept {
    ObjectRegistry source(allocator, types, {0x5648, 1});
    ObjectRegistry target(allocator, types, {0x5648, 2});
    ErrorCode error = ErrorCode::None;
    {
        RootRef<ValidHolder> holder = New(source);
        FillValid(types, source, holder.Handle(), allocator);
        With(source, holder.Handle(), edit);
        auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        CHECK(saved);
        if (saved) {
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            error = loaded ? ErrorCode::None : loaded.Error();
        } else {
            error = saved.Error();
        }
    }

    CHECK(source.Shutdown(source.OwnerAccess()));
    CHECK(target.Shutdown(target.OwnerAccess()));
    return error;
}

} // namespace

// 유효한 값은 모든 자리가 왕복하고, 중첩 struct는 안쪽 hook이 먼저다(ValidBox: 멤버 range의 r 다음
// b, ValidDerived: base hook r 다음 d).
TEST(ValidateHookGraph_ValidValuesLoadInnerFirst) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5647, 1});
        ObjectRegistry target(allocator, types, {0x5647, 2});
        {
            RootRef<ValidHolder> holder = New(source);
            FillValid(types, source, holder.Handle(), allocator);
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            CurrentValidateLog().Clear();
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            // box(r b), ranges 둘(r r), maybe(r), byKey 값(r), any 내용(r), fixed 둘(r r),
            // derived(r d), elements 원소의 range(r), outer.list 기본값(l)이 한 번씩이다(저장하지
            // 않는 C++ 생성 값은 부르지 않는다 — 기록 수가 정확히 자리 수다).
            CHECK(CurrentValidateLog().count == 13);
            const auto& log = CurrentValidateLog();
            bool boxOrder = false;
            bool derivedOrder = false;
            for (usize index = 0; index + 1 < log.count && index + 1 < sizeof(log.calls); ++index) {
                boxOrder = boxOrder || (log.calls[index] == 'r' && log.calls[index + 1] == 'b');
                derivedOrder =
                    derivedOrder || (log.calls[index] == 'r' && log.calls[index + 1] == 'd');
            }

            CHECK(boxOrder && derivedOrder);
            With(target, loaded.Value().Handle(), [](ValidHolder& value) noexcept {
                CHECK(value.box.range.low == 1 && value.box.range.high == 2 &&
                      value.box.limit == 10);
                REQUIRE(value.ranges.Size() == 2);
                CHECK(value.ranges[1].low == 3 && value.ranges[1].high == 4);
                REQUIRE(value.maybe.HasValue());
                CHECK(value.maybe.Value().low == 5);
                const ValidRange* entry = value.byKey.Find(7);
                REQUIRE(entry);
                CHECK(entry->high == 8);
                REQUIRE(value.any.Get<ValidRange>());
                CHECK(value.any.Get<ValidRange>()->high == 10);
                CHECK(value.fixed[1].high == 3);
                CHECK(value.derived.extra == 3);
                REQUIRE(value.elements.Size() == 1);
                CHECK(value.elements[0].range.high == 5);
                CHECK(value.plain == 11);
            });
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}

// 자리마다 hook이 거부하면 복원 전체가 InvalidArgument다. C++가 직접 만든 잘못된 값은 저장은
// 되지만(encode는 hook을 부르지 않는다) 복원이 막는다.
TEST(ValidateHookGraph_RejectedValueFailsWholeLoad) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    CHECK(LoadWith(allocator, types, [](ValidHolder&) noexcept {}) == ErrorCode::None);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              value.box.range = Range(3, 2);
          }) == ErrorCode::CorruptData);
    // 바깥 hook(range.high ≤ limit)의 다른 오류 코드(Overflow)도 InvalidArgument다.
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept { value.box.limit = 1; }) ==
          ErrorCode::CorruptData);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              value.ranges[1] = Range(9, 0);
          }) == ErrorCode::CorruptData);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              value.maybe.Emplace(Range(9, 0));
          }) == ErrorCode::CorruptData);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              ValidRange* entry = value.byKey.Find(7);
              REQUIRE(entry);
              *entry = Range(9, 0);
          }) == ErrorCode::CorruptData);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              *value.any.Get<ValidRange>() = Range(9, 0);
          }) == ErrorCode::CorruptData);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              value.fixed[1] = Range(9, 0);
          }) == ErrorCode::CorruptData);
    // base hook(ValidRange)과 파생 hook(ValidDerived) 모두 본다.
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept { value.derived.low = 9; }) ==
          ErrorCode::CorruptData);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              value.derived.extra = -1;
          }) == ErrorCode::CorruptData);
    CHECK(LoadWith(allocator, types, [](ValidHolder& value) noexcept {
              value.elements[0].range = Range(9, 0);
          }) == ErrorCode::CorruptData);
}

// 거부된 복원은 대상 Registry에 아무것도 남기지 않는다: 같은 Registry에 이어서 유효한 그래프를
// 복원할 수 있고, 거부된 복원의 객체는 이름·handle로 보이지 않는다.
TEST(ValidateHookGraph_RejectedLoadPublishesNothing) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5649, 1});
        ObjectRegistry target(allocator, types, {0x5649, 2});
        {
            RootRef<ValidHolder> holder = New(source);
            FillValid(types, source, holder.Handle(), allocator);
            auto valid = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(valid);
            With(source, holder.Handle(),
                 [](ValidHolder& value) noexcept { value.fixed[0] = Range(5, 1); });
            auto invalid =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(invalid);
            const uint32 before = target.Stats().liveObjects;
            auto rejected = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                            invalid.Value().View());
            CHECK(!rejected && rejected.Error() == ErrorCode::CorruptData);
            CHECK(target.Stats().liveObjects == before);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          valid.Value().View());
            REQUIRE(loaded);
            CHECK(target.Stats().liveObjects == before + 1);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}
} // namespace validate_hook_graph_tests_file
