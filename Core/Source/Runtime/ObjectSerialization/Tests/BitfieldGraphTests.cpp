#include <ReflectionTestFixtures/BitfieldFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9m bitfield bool PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 14절, 17절
// B9m 항목, fixture ReflectionTestFixtures/BitfieldFixture.h): 형식 4종(uint8·16·32·64)·클래스
// 필드·private 필드·struct 멤버·Array<S> 원소·상속 필드의 그래프 왕복과 다시 저장한 byte 같음, 경로
// 읽기·쓰기와 이웃 비트 보존(같은 바이트의 NATIVE bitfield·다른 PROPERTY bitfield·일반 멤버),
// ReadOnly 쓰기 거부.
using namespace rocks;
namespace bitfield_graph_tests_file {

using reflection_fixture::BitfieldChild;
using reflection_fixture::BitfieldFlags;
using reflection_fixture::BitfieldHolder;

namespace {

template <class T>
Guid FieldOf(StringView name) noexcept {
    for (const FieldSchema& field : GeneratedType<T>::Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.id;
        }
    }

    platform::Fatal("fixture field missing");
}

bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<BitfieldFlags>::Register(types).IsOk() &&
           GeneratedType<BitfieldHolder>::Register(types).IsOk() &&
           GeneratedType<BitfieldChild>::Register(types).IsOk();
}

template <class T>
RootRef<T> New(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<T>(objects.OwnerAccess(), TypeIdOf<T>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

template <class T, class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

FieldValue Bool(Allocator& allocator, bool value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Bool;
    result.boolean = value;
    return result;
}

// 문자열 경로로 bool을 쓰고 다시 읽는다(실패하면 시험 실패).
bool WriteRead(ObjectRegistry& objects, ObjectHandle handle, StringView text, bool value) noexcept {
    SystemAllocator scratch;
    PropertyPathBuffer path;
    FieldValue read(scratch);
    const bool done =
        ParsePropertyPath(objects, objects.OwnerAccess(), handle, text, path) &&
        PathWrite(objects, objects.OwnerAccess(), handle, path.View(), Bool(scratch, value)) &&
        PathRead(objects, objects.OwnerAccess(), handle, path.View(), read);
    CHECK(done && read.kind == ValueKind::Bool);
    return read.boolean;
}

void Fill(BitfieldHolder& value) noexcept {
    value.on = 1;
    value.big = 1;
    value.SetHidden(true);
    value.flags.first = 1;
    value.flags.wide = 1;
    value.flags.widest = 1;
    value.flags.plain = 200;
    REQUIRE(value.list.TryPushBack(BitfieldFlags{}));
    REQUIRE(value.list.TryPushBack(BitfieldFlags{}));
    value.list[0].first = 1;
    value.list[0].widest = 1;
    value.list[1].second = 1;
    value.list[1].wider = 1;
}

void CheckFilled(const BitfieldHolder& value) noexcept {
    CHECK(value.on == 1 && value.off == 0 && value.big == 1 && value.locked == 0);
    CHECK(value.Hidden());
    CHECK(value.flags.first == 1 && value.flags.second == 0 && value.flags.wide == 1 &&
          value.flags.wider == 0 && value.flags.widest == 1 && value.flags.plain == 200);
    REQUIRE(value.list.Size() == 2);
    CHECK(value.list[0].first == 1 && value.list[0].second == 0 && value.list[0].widest == 1);
    CHECK(value.list[1].first == 0 && value.list[1].second == 1 && value.list[1].wider == 1);
}

} // namespace

// 모든 자리의 bitfield가 왕복하고 다시 저장한 byte가 같다. NATIVE bitfield는 저장하지
// 않는다(복원하면 기본값).
TEST(BitfieldGraph_RoundTripEveryPosition) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x4246, 1});
        ObjectRegistry target(allocator, types, {0x4246, 2});
        {
            RootRef<BitfieldChild> holder = New<BitfieldChild>(source);
            With<BitfieldChild>(source, holder.Handle(), [](BitfieldChild& value) noexcept {
                Fill(value);
                value.extra = 1;
                value.cache = 45;
                value.flags.spare = 37;
            });
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            With<BitfieldChild>(target, loaded.Value().Handle(), [](BitfieldChild& value) noexcept {
                CheckFilled(value);
                CHECK(value.extra == 1);
                CHECK(value.cache == 0 && value.flags.spare == 0);
            });
            auto again =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 경로 읽기·쓰기(클래스 필드·상속 필드·private 필드·struct 멤버·Array 원소 멤버)는 그 비트만 바꾸고
// 같은 바이트의 이웃(NATIVE bitfield·다른 PROPERTY bitfield·일반 멤버)을 보존한다. ReadOnly 필드
// 쓰기는 거부한다.
TEST(BitfieldPath_WritesPreserveNeighbours) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4246, 3});
        RootRef<BitfieldChild> holder = New<BitfieldChild>(objects);
        const ObjectHandle handle = holder.Handle();
        With<BitfieldChild>(objects, handle, [](BitfieldChild& value) noexcept {
            value.on = 1;
            value.cache = 45;
            value.flags.first = 1;
            value.flags.spare = 37;
            value.flags.plain = 200;
            REQUIRE(value.list.TryPushBack(BitfieldFlags{}));
            value.list[0].spare = 21;
        });

        CHECK(WriteRead(objects, handle, "off", true));
        CHECK(WriteRead(objects, handle, "extra", true));
        CHECK(WriteRead(objects, handle, "hidden", true));
        CHECK(WriteRead(objects, handle, "flags.second", true));
        CHECK(WriteRead(objects, handle, "flags.widest", true));
        CHECK(WriteRead(objects, handle, "list[0].wide", true));
        CHECK(!WriteRead(objects, handle, "on", false));
        With<BitfieldChild>(objects, handle, [](BitfieldChild& value) noexcept {
            CHECK(value.on == 0 && value.off == 1 && value.cache == 45 && value.extra == 1);
            CHECK(value.Hidden());
            CHECK(value.flags.first == 1 && value.flags.second == 1 && value.flags.spare == 37 &&
                  value.flags.plain == 200 && value.flags.widest == 1);
            CHECK(value.list[0].wide == 1 && value.list[0].spare == 21);
        });

        // ReadOnly 필드는 경로로 쓸 수 없다(읽기는 된다).
        PropertyPathBuffer locked;
        REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "locked", locked));
        CHECK(!PathWrite(objects, objects.OwnerAccess(), handle, locked.View(),
                         Bool(allocator, true)));
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, objects.OwnerAccess(), handle, locked.View(), read));
        CHECK(read.kind == ValueKind::Bool && !read.boolean);

        // bool이 아닌 값은 TypeMismatch다.
        FieldValue wrong(allocator);
        wrong.kind = ValueKind::UInt8;
        wrong.unsignedInteger = 1;
        PropertyPathBuffer on;
        REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "on", on));
        CHECK(!PathWrite(objects, objects.OwnerAccess(), handle, on.View(), wrong));
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 스키마: bitfield는 Bool 단일 값 필드이고(종류·대상 없음), 상속 필드는 base의 필드 ID를 그대로
// 쓴다.
TEST(BitfieldSchema_IsBoolAndInherited) {
    const TypeSchema& holder = GeneratedType<BitfieldHolder>::Schema();
    const TypeSchema& child = GeneratedType<BitfieldChild>::Schema();
    for (const FieldSchema& field : holder.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == "on") {
            CHECK(field.value.kind == ValueKind::Bool && field.value.type == TypeId{});
        }
    }

    CHECK(child.fields.Size() == holder.fields.Size() + 1);
    CHECK(FieldOf<BitfieldChild>("on") == FieldOf<BitfieldHolder>("on"));
    CHECK(FieldOf<BitfieldChild>("hidden") == FieldOf<BitfieldHolder>("hidden"));
}
} // namespace bitfield_graph_tests_file
