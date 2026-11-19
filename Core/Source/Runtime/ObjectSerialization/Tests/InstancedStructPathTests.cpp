#include "InstancedStructTestSupport.h"

// 리플렉션 B9j InstancedStruct의 경로 시험이다(Docs/Design/ReflectionExtensions.ko.md 11절, 17절
// B9j 항목): 형 바꾸기(PathSetInstancedType)·비우기(PathClearInstanced)·담긴 타입 조회
// (PathReadInstancedType), 지금 타입으로 해석하는 멤버 경로(`value.x`·`list[0].x`·`table[3].x`·
// `fixed[1].x`·`host.value.x`)의 읽기·쓰기, 객체 문맥 판 문자열 경로 해석·출력, 형 바꾸기의 거부
// (미등록·담을 수 없는 struct·깊이 합·ReadOnly·InstancedStruct가 아닌 끝), 통째 읽기 거부.
using namespace rocks;
using namespace rocks::test::instanced;

namespace {

FieldValue I32(Allocator& allocator, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Int32;
    result.signedInteger = value;
    return result;
}

// 문자열 경로를 객체 문맥으로 해석한다(실패하면 시험 실패).
PropertyPathBuffer Parse(ObjectRegistry& objects, ObjectHandle holder, StringView text) noexcept {
    PropertyPathBuffer path;
    auto parsed = ParsePropertyPath(objects, objects.OwnerAccess(), holder, text, path);
    CHECK(parsed);
    return path;
}

// 문자열 경로로 int32 값을 쓰고 다시 읽어 돌려준다(실패하면 시험 실패와 -1).
int64 WriteRead(ObjectRegistry& objects, ObjectHandle holder, StringView text,
                int64 value) noexcept {
    SystemAllocator scratch;
    const PropertyPathBuffer path = Parse(objects, holder, text);
    if (!PathWrite(objects, objects.OwnerAccess(), holder, path.View(), I32(scratch, value))) {
        CHECK(false);
        return -1;
    }

    FieldValue read(scratch);
    if (!PathRead(objects, objects.OwnerAccess(), holder, path.View(), read)) {
        CHECK(false);
        return -1;
    }

    return read.signedInteger;
}

} // namespace

// 형 바꾸기 뒤 멤버를 지금 타입으로 읽고 쓰고, 담긴 타입을 조회하며, 비우면 멤버 단계가
// NotFound(InstancedEmpty)다. 같은 자리를 다른 타입으로 바꾸면 그 타입의 멤버 경로만 풀린다.
TEST(InstancedStructPath_SetTypeMembersAndClear) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495350, 1});
        RootRef<InstancedHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        const PathStep valueStep[] = {{FieldOf<InstancedHolder>("value")}};
        const PropertyPath value{valueStep};
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, value,
                                     TypeIdOf<InstancedPoint>()));
        CHECK(WriteRead(objects, handle, "value.x", 12) == 12);
        With(objects, handle, [](InstancedHolder& held) noexcept {
            REQUIRE(held.value.Get<InstancedPoint>());
            CHECK(held.value.Get<InstancedPoint>()->x == 12);
        });

        auto type = PathReadInstancedType(objects, objects.OwnerAccess(), handle, value);
        REQUIRE(type);
        CHECK(type.Value().IsValid() && type.Value().Id() == TypeIdOf<InstancedPoint>());

        // 문자열 경로 왕복(객체 문맥 판 — 지금 타입의 도구 이름).
        const PropertyPathBuffer member = Parse(objects, handle, "VALUE.x");
        String text(allocator);
        REQUIRE(FormatPropertyPath(objects, objects.OwnerAccess(), handle, member.View(), text));
        CHECK(text.View() == "value.x");
        // lease 판은 InstancedStruct 뒤 멤버를 풀 수 없다.
        auto holderType = types.Acquire(TypeIdOf<InstancedHolder>());
        REQUIRE(holderType);
        PropertyPathBuffer plain;
        PropertyPathFailure failure;
        CHECK(!ParsePropertyPath(holderType.Value(), "value.x", plain, &failure));
        CHECK(failure.reason == PropertyPathError::NotStruct);

        // 통째 읽기는 aggregate다.
        FieldValue whole(allocator);
        auto read = PathRead(objects, objects.OwnerAccess(), handle, value, whole, &failure);
        CHECK(!read && read.Error() == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::AggregateValue);

        // 다른 타입으로 바꾸면 옛 멤버는 없다(UnknownField).
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, value,
                                     TypeIdOf<InstancedVersioned>()));
        auto stale = PathWrite(objects, objects.OwnerAccess(), handle, member.View(),
                               I32(allocator, 1), nullptr, &failure);
        CHECK(!stale && stale.Error() == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::UnknownField);
        CHECK(WriteRead(objects, handle, "value.extra", 4) == 4);

        REQUIRE(PathClearInstanced(objects, objects.OwnerAccess(), handle, value));
        auto empty = PathReadInstancedType(objects, objects.OwnerAccess(), handle, value);
        REQUIRE(empty);
        CHECK(!empty.Value().IsValid());
        auto missing = PathWrite(objects, objects.OwnerAccess(), handle, member.View(),
                                 I32(allocator, 1), nullptr, &failure);
        CHECK(!missing && missing.Error() == ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::InstancedEmpty);
        PropertyPathBuffer unresolved;
        CHECK(!ParsePropertyPath(objects, objects.OwnerAccess(), handle, "value.x", unresolved,
                                 &failure));
        CHECK(failure.reason == PropertyPathError::InstancedEmpty);
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 원소·정적 배열 원소·Map 값·struct 멤버 자리: Array 삽입은 빈 값을 넣고(값을 주면
// Unsupported(AggregateValue)), Map PathInsertKey는 빈 값을 넣으며, 각 자리에서 형 바꾸기와 멤버
// 쓰기가 된다.
TEST(InstancedStructPath_ElementsMapValuesAndStructMembers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495350, 2});
        RootRef<InstancedHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        PathStep listStep[] = {{FieldOf<InstancedHolder>("list"), 0, PathSelector::Index}};
        REQUIRE(PathInsertElement(objects, objects.OwnerAccess(), handle, PropertyPath{listStep},
                                  nullptr));
        FieldValue given = I32(allocator, 1);
        PropertyPathFailure failure;
        auto withValue = PathInsertElement(objects, objects.OwnerAccess(), handle,
                                           PropertyPath{listStep}, &given, &failure);
        CHECK(!withValue && failure.reason == PropertyPathError::AggregateValue);
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, PropertyPath{listStep},
                                     TypeIdOf<InstancedPoint>()));
        CHECK(WriteRead(objects, handle, "list[0].y", 21) == 21);

        const PathStep fixedStep[] = {{FieldOf<InstancedHolder>("fixed"), 1, PathSelector::Index}};
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle,
                                     PropertyPath{fixedStep}, TypeIdOf<InstancedPoint>()));
        CHECK(WriteRead(objects, handle, "fixed[1].x", 31) == 31);

        const PropertyPathBuffer tableEntry = Parse(objects, handle, "table[3]");
        auto inserted = PathInsertKey(objects, objects.OwnerAccess(), handle, tableEntry.View());
        REQUIRE(inserted);
        CHECK(inserted.Value());
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, tableEntry.View(),
                                     TypeIdOf<InstancedPoint>()));
        CHECK(WriteRead(objects, handle, "table[3].x", 41) == 41);

        const PropertyPathBuffer host = Parse(objects, handle, "host.value");
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, host.View(),
                                     TypeIdOf<InstancedPoint>()));
        CHECK(WriteRead(objects, handle, "host.value.x", 51) == 51);

        With(objects, handle, [](InstancedHolder& value) noexcept {
            CHECK(value.list.Size() == 1 && value.list[0].Get<InstancedPoint>() &&
                  value.list[0].Get<InstancedPoint>()->y == 21);
            CHECK(value.fixed[1].Get<InstancedPoint>() &&
                  value.fixed[1].Get<InstancedPoint>()->x == 31);
            CHECK(value.table.Find(3u) && value.table.Find(3u)->Get<InstancedPoint>() &&
                  value.table.Find(3u)->Get<InstancedPoint>()->x == 41);
            CHECK(value.host.value.Get<InstancedPoint>() &&
                  value.host.value.Get<InstancedPoint>()->x == 51);
        });

        REQUIRE(PathClearInstanced(objects, objects.OwnerAccess(), handle, PropertyPath{listStep}));
        REQUIRE(PathRemoveElement(objects, objects.OwnerAccess(), handle, PropertyPath{listStep}));
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 형 바꾸기의 거부: 미등록 struct NotFound, 클래스·담을 수 없는 struct InvalidArgument(사유
// InvalidValue), 위치 깊이 + 담긴 깊이 > 16(host.value에 깊이 16) InvalidArgument, ReadOnly 필드
// Unsupported(ReadOnly), InstancedStruct가 아닌 끝 TypeMismatch(NotInstanced). 실패하면 값은
// 그대로다.
TEST(InstancedStructPath_SetTypeRejections) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495350, 3});
        RootRef<InstancedHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        const PathStep valueStep[] = {{FieldOf<InstancedHolder>("value")}};
        const PropertyPath value{valueStep};
        PropertyPathFailure failure;
        auto unknown = PathSetInstancedType(objects, objects.OwnerAccess(), handle, value,
                                            TypeId{0x495350, 99}, &failure);
        CHECK(!unknown && unknown.Error() == ErrorCode::NotFound);
        const TypeId rejected[] = {TypeIdOf<InstancedHolder>(), TypeIdOf<InstancedRefHolder>(),
                                   TypeIdOf<InstancedRequiredLink>(), TypeIdOf<InstancedNested>()};
        for (const TypeId& type : rejected) {
            auto set =
                PathSetInstancedType(objects, objects.OwnerAccess(), handle, value, type, &failure);
            CHECK(!set && set.Error() == ErrorCode::InvalidArgument);
            CHECK(failure.reason == PropertyPathError::InvalidValue);
        }

        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, value,
                                     TypeIdOf<InstancedDepth16>()));
        const PropertyPathBuffer host = Parse(objects, handle, "host.value");
        auto deep = PathSetInstancedType(objects, objects.OwnerAccess(), handle, host.View(),
                                         TypeIdOf<InstancedDepth16>(), &failure);
        CHECK(!deep && deep.Error() == ErrorCode::InvalidArgument);
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, host.View(),
                                     TypeIdOf<InstancedDepth15>()));

        const PathStep lockedStep[] = {{FieldOf<InstancedHolder>("locked")}};
        auto locked =
            PathSetInstancedType(objects, objects.OwnerAccess(), handle, PropertyPath{lockedStep},
                                 TypeIdOf<InstancedPoint>(), &failure);
        CHECK(!locked && locked.Error() == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        CHECK(ErrorOf(PathClearInstanced(objects, objects.OwnerAccess(), handle,
                                         PropertyPath{lockedStep})) == ErrorCode::Unsupported);

        const PropertyPathBuffer count = Parse(objects, handle, "host.count");
        auto notInstanced =
            PathSetInstancedType(objects, objects.OwnerAccess(), handle, count.View(),
                                 TypeIdOf<InstancedPoint>(), &failure);
        CHECK(!notInstanced && notInstanced.Error() == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotInstanced);
        With(objects, handle, [](InstancedHolder& held) noexcept {
            CHECK(held.value.Get<InstancedDepth16>() != nullptr);
            CHECK(held.host.value.Get<InstancedDepth15>() != nullptr);
            CHECK(held.locked.IsEmpty());
        });

        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 담기 조건의 나머지 둘(B9j–B9m 묶음 리뷰의 시험 공백): Registry 컨테이너(ObjectRefArray)를 담은
// struct와 WorldOnly 소유자를 요구하는 struct는 형 바꾸기·lease 판 Emplace가 InvalidArgument다.
TEST(InstancedStructPath_RegistryContainerAndWorldOnlyContentRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x495350, 9});
        RootRef<InstancedHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        const PathStep valueStep[] = {{FieldOf<InstancedHolder>("value")}};
        const TypeId rejected[] = {TypeIdOf<InstancedRefArrayHolder>(),
                                   TypeIdOf<InstancedWorldBound>()};
        for (const TypeId type : rejected) {
            PropertyPathFailure failure;
            auto set = PathSetInstancedType(objects, objects.OwnerAccess(), handle,
                                            PropertyPath{valueStep}, type, &failure);
            CHECK(!set && set.Error() == ErrorCode::InvalidArgument);
            CHECK(failure.reason == PropertyPathError::InvalidValue);
            auto lease = types.AcquireValueType(type, TypeKind::Struct);
            REQUIRE(lease);
            InstancedStruct<> value;
            auto made = value.Emplace(Move(lease.Value()), allocator);
            CHECK(!made && made.Error() == ErrorCode::InvalidArgument);
            CHECK(value.IsEmpty());
        }

        With(objects, handle,
             [](InstancedHolder& value) noexcept { CHECK(value.value.IsEmpty()); });
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
