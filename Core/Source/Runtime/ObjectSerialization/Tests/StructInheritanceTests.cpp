#include "InstancedStructTestSupport.h"

#include <ReflectionTestFixtures/StructInheritanceFixture.generated.h>

// 리플렉션 B9k struct 상속 시험이다(Docs/Design/ReflectionExtensions.ko.md 12절, fixture
// ReflectionTestFixtures/StructInheritanceFixture.h): base 필드 펼침(필드 ID는 선언한 struct
// 소유)과 생성 baseType, 형질(Atomic·강한 참조·Required) 상속, 스키마 사슬 IsA, base의 Unregister
// Busy, 등록 때의 base 검사, 그래프 왕복(오프셋이 0이 아닌 base 하위 객체 포함), base 멤버 경로
// 쓰기, InstancedStruct<Base>의 파생 수용과 무관한 struct 거부(Emplace·형 바꾸기·decode).
using namespace rocks;
namespace struct_inheritance_tests_file {

using namespace rocks::test::instanced;
using reflection_fixture::InheritAtomicBase;
using reflection_fixture::InheritAtomicLeaf;
using reflection_fixture::InheritBase;
using reflection_fixture::InheritHolder;
using reflection_fixture::InheritLeaf;
using reflection_fixture::InheritMiddle;
using reflection_fixture::InheritNestedBase;
using reflection_fixture::InheritNestedLeaf;
using reflection_fixture::InheritOffset;
using reflection_fixture::InheritOther;
using reflection_fixture::InheritPrivateBase;
using reflection_fixture::InheritPrivateLeaf;
using reflection_fixture::InheritRefBase;
using reflection_fixture::InheritRefLeaf;
using reflection_fixture::InheritRequiredBase;
using reflection_fixture::InheritRequiredLeaf;
using reflection_fixture::InheritShapeChild;
using reflection_fixture::InheritShapeHost;

static_assert(GeneratedType<InheritRefLeaf>::ContainsStrongReference,
              "strong reference trait is inherited from the base");
static_assert(GeneratedType<InheritRequiredLeaf>::ContainsRequired,
              "Required trait is inherited from the base");
// 상속된 형질(B9j–B9m 묶음 리뷰의 시험 공백): InstancedStruct 담기와 깊이는 base에서 온다.
static_assert(GeneratedType<InheritShapeChild>::ContainsInstancedStruct,
              "InstancedStruct trait is inherited from the base");
static_assert(GeneratedType<InheritNestedLeaf>::Depth == 2 &&
                  GeneratedType<InheritNestedLeaf>::Depth ==
                      GeneratedType<InheritNestedBase>::Depth,
              "nesting depth is inherited from the base");
static_assert(!GeneratedType<InheritLeaf>::ContainsStrongReference &&
                  !GeneratedType<InheritLeaf>::ContainsRequired,
              "plain base chain has no reference or Required traits");

namespace {

// InheritBase를 뺀 나머지 fixture를 base 먼저 등록한다(base 토큰은 호출자가 쥔다).
bool RegisterDerived(TypeRegistry& types) noexcept {
    using namespace reflection_fixture;
    return GeneratedType<InheritMiddle>::Register(types).IsOk() &&
           GeneratedType<InheritLeaf>::Register(types).IsOk() &&
           GeneratedType<InheritOffset>::Register(types).IsOk() &&
           GeneratedType<InheritAtomicBase>::Register(types).IsOk() &&
           GeneratedType<InheritAtomicLeaf>::Register(types).IsOk() &&
           GeneratedType<InheritRefBase>::Register(types).IsOk() &&
           GeneratedType<InheritRefLeaf>::Register(types).IsOk() &&
           GeneratedType<InheritRequiredBase>::Register(types).IsOk() &&
           GeneratedType<InheritRequiredLeaf>::Register(types).IsOk() &&
           GeneratedType<InheritOther>::Register(types).IsOk() &&
           GeneratedType<InheritShapeHost>::Register(types).IsOk() &&
           GeneratedType<InheritShapeChild>::Register(types).IsOk() &&
           GeneratedType<InheritNestedBase>::Register(types).IsOk() &&
           GeneratedType<InheritNestedLeaf>::Register(types).IsOk() &&
           GeneratedType<InheritPrivateBase>::Register(types).IsOk() &&
           GeneratedType<InheritPrivateLeaf>::Register(types).IsOk() &&
           GeneratedType<InheritHolder>::Register(types).IsOk();
}

bool RegisterAll(TypeRegistry& types) noexcept {
    return GeneratedType<InheritBase>::Register(types).IsOk() && RegisterDerived(types);
}

RootRef<InheritHolder> NewHolder(ObjectRegistry& objects) noexcept {
    auto created =
        objects.NewObject<InheritHolder>(objects.OwnerAccess(), TypeIdOf<InheritHolder>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

template <class Edit>
void WithHolder(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<InheritHolder>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

template <class S>
S& MakeShape(ObjectRegistry& objects, InstancedStruct<InheritBase>& value) noexcept {
    if (!value.Emplace<S>(objects.Types(), objects.ObjectAllocator())) {
        platform::Fatal("test instanced emplace failed");
    }

    return *value.Get<S>();
}

// 모든 상속 자리를 채운다.
void FillHolder(ObjectRegistry& objects, ObjectHandle holder) noexcept {
    WithHolder(objects, holder, [&](InheritHolder& value) noexcept {
        value.leaf.a = 1;
        REQUIRE(value.leaf.name.TryAssign("leaf"));
        value.leaf.b = 2;
        value.leaf.c = 3.5f;
        value.offset.a = 4;
        REQUIRE(value.offset.name.TryAssign("off"));
        value.offset.d = 5;
        value.atomic.x = 6;
        value.atomic.y = 7;
        value.atomic.z = 8;
        value.link.n = 9;
        InheritLeaf& leaf = MakeShape<InheritLeaf>(objects, value.shape);
        leaf.a = 10;
        leaf.c = 1.5f;
        REQUIRE(value.shapes.TryEmplaceBack());
        REQUIRE(value.shapes.TryEmplaceBack());
        REQUIRE(value.shapes.TryEmplaceBack());
        MakeShape<InheritMiddle>(objects, value.shapes[0]).b = 11;
        MakeShape<InheritBase>(objects, value.shapes[2]).a = 12;
        MakeShape<InheritOffset>(objects, value.host.shape).d = 13;
        value.host.count = 14;
    });
}

void CheckHolder(ObjectRegistry& objects, ObjectHandle holder) noexcept {
    WithHolder(objects, holder, [](InheritHolder& value) noexcept {
        CHECK(value.leaf.a == 1 && value.leaf.name.View() == "leaf" && value.leaf.b == 2u &&
              value.leaf.c == 3.5f);
        CHECK(value.offset.a == 4 && value.offset.name.View() == "off" && value.offset.d == 5);
        CHECK(value.atomic.x == 6 && value.atomic.y == 7 && value.atomic.z == 8);
        CHECK(value.link.n == 9 && value.link.target.Handle().IsNull());
        const InheritLeaf* leaf = value.shape.Get<InheritLeaf>();
        REQUIRE(leaf);
        CHECK(leaf->a == 10 && leaf->c == 1.5f);
        REQUIRE(value.shapes.Size() == 3);
        CHECK(value.shapes[0].Get<InheritMiddle>() &&
              value.shapes[0].Get<InheritMiddle>()->b == 11u);
        CHECK(value.shapes[1].IsEmpty());
        CHECK(value.shapes[2].Get<InheritBase>() && value.shapes[2].Get<InheritBase>()->a == 12);
        CHECK(value.host.shape.Get<InheritOffset>() &&
              value.host.shape.Get<InheritOffset>()->d == 13);
        CHECK(value.host.count == 14u);
    });
}

// shape 필드 payload가 content인 InheritHolder 그래프다.
Array<uint8> ShapeGraph(Allocator& allocator, Span<const uint8> content) noexcept {
    const FieldBytes fields[] = {
        {FieldOf<InheritHolder>("shape"), static_cast<uint32>(WireType::InstancedStruct), content}};
    auto record =
        RecordOf(allocator, TypeIdOf<InheritHolder>(), TypeOf<InheritHolder>().version, fields);
    auto graph = test::graph::Single(allocator, TypeIdOf<InheritHolder>(), record.View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    return Move(graph.Value());
}

FieldValue I32(Allocator& allocator, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Int32;
    result.signedInteger = value;
    return result;
}

} // namespace

// 생성 스키마는 base 필드를 앞에 펼쳐 담고(필드 ID는 선언한 struct 소유) baseType은 직접 base다.
// 비리플렉션 빈 base는 스키마에 나타나지 않는다. Atomic은 base에서 상속된다.
TEST(StructInheritance_SchemaFlattensBaseFields) {
    const TypeSchema& base = TypeOf<InheritBase>();
    const TypeSchema& middle = TypeOf<InheritMiddle>();
    const TypeSchema& leaf = TypeOf<InheritLeaf>();
    CHECK(base.baseType == TypeId{});
    CHECK(middle.baseType == TypeIdOf<InheritBase>());
    CHECK(leaf.baseType == TypeIdOf<InheritMiddle>());
    CHECK(TypeOf<InheritOffset>().baseType == TypeIdOf<InheritBase>());
    CHECK(base.fields.Size() == 2 && middle.fields.Size() == 3 && leaf.fields.Size() == 4);
    CHECK(FieldOf<InheritLeaf>("a") == FieldOf<InheritBase>("a"));
    CHECK(FieldOf<InheritLeaf>("name") == FieldOf<InheritBase>("name"));
    CHECK(FieldOf<InheritLeaf>("b") == FieldOf<InheritMiddle>("b"));
    CHECK(FieldOf<InheritOffset>("a") == FieldOf<InheritBase>("a"));
    CHECK((TypeOf<InheritAtomicLeaf>().flags & static_cast<uint32>(TypeFlags::Atomic)) != 0);
}

// 등록은 base 레코드를 의존으로 붙잡고(Unregister Busy), lease의 IsA가 스키마 사슬을 보며, 형질
// (Atomic·강한 참조·Required)은 펼친 필드와 base에서 상속된다.
TEST(StructInheritance_RegistrationChainAndTraits) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto baseToken = GeneratedType<InheritBase>::Register(types);
    REQUIRE(baseToken);
    REQUIRE(RegisterDerived(types));
    {
        auto leaf = types.AcquireValueType(TypeIdOf<InheritLeaf>(), TypeKind::Struct);
        REQUIRE(leaf);
        CHECK(leaf.Value().IsA(TypeIdOf<InheritLeaf>()));
        CHECK(leaf.Value().IsA(TypeIdOf<InheritMiddle>()));
        CHECK(leaf.Value().IsA(TypeIdOf<InheritBase>()));
        CHECK(!leaf.Value().IsA(TypeIdOf<InheritOther>()));
        auto base = types.AcquireValueType(TypeIdOf<InheritBase>(), TypeKind::Struct);
        REQUIRE(base);
        CHECK(!base.Value().IsA(TypeIdOf<InheritLeaf>()));
        auto atomic = types.AcquireValueType(TypeIdOf<InheritAtomicLeaf>(), TypeKind::Struct);
        REQUIRE(atomic);
        CHECK(atomic.Value().Struct().IsAtomic());
        auto reference = types.AcquireValueType(TypeIdOf<InheritRefLeaf>(), TypeKind::Struct);
        REQUIRE(reference);
        CHECK(reference.Value().Struct().ContainsStrongReference());
        auto required = types.AcquireValueType(TypeIdOf<InheritRequiredLeaf>(), TypeKind::Struct);
        REQUIRE(required);
        CHECK(required.Value().Struct().ContainsRequired());
    }

    CHECK(ErrorOf(types.Unregister(baseToken.Value())) == ErrorCode::Busy);
    CHECK(types.Shutdown());
}

// 등록 때의 base 검사: 미등록 base NotFound, 자기 자신 base InvalidArgument, base 필드를 같은 ID로
// 담지 않은 파생 TypeMismatch, Atomic base 아래의 Atomic이 아닌 파생 InvalidArgument. 생성
// 스키마를 고친 사본을 원래 native 타입으로 등록한다(그 native 타입은 아직 등록하지 않는다).
TEST(StructInheritance_RegisterStructChecksBase) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto early = GeneratedType<InheritMiddle>::Register(types);
    CHECK(!early && early.Error() == ErrorCode::NotFound);
    REQUIRE(GeneratedType<InheritBase>::Register(types));
    REQUIRE(GeneratedType<InheritOther>::Register(types));
    REQUIRE(GeneratedType<InheritAtomicBase>::Register(types));

    TypeSchema self = TypeOf<InheritMiddle>();
    self.baseType = self.id;
    auto own = types.RegisterStruct(self, GeneratedType<InheritMiddle>::Ops());
    CHECK(!own && own.Error() == ErrorCode::InvalidArgument);

    TypeSchema unrelated = TypeOf<InheritMiddle>();
    unrelated.baseType = TypeIdOf<InheritOther>();
    auto mismatch = types.RegisterStruct(unrelated, GeneratedType<InheritMiddle>::Ops());
    CHECK(!mismatch && mismatch.Error() == ErrorCode::TypeMismatch);

    TypeSchema plain = TypeOf<InheritAtomicLeaf>();
    plain.flags = 0;
    auto notAtomic = types.RegisterStruct(plain, GeneratedType<InheritAtomicLeaf>::Ops());
    CHECK(!notAtomic && notAtomic.Error() == ErrorCode::InvalidArgument);

    // 고치지 않은 스키마는 그대로 등록된다.
    CHECK(GeneratedType<InheritMiddle>::Register(types));
    CHECK(GeneratedType<InheritAtomicLeaf>::Register(types));
    CHECK(types.Shutdown());
}

// 상속 struct 필드(오프셋이 0이 아닌 base 하위 객체 포함)·Atomic·참조 base와
// InstancedStruct<InheritBase>(필드·원소·struct 멤버)의 파생 값이 그래프로 왕복하고, 다시 저장한
// byte가 같다.
TEST(StructInheritanceGraph_RoundTrip) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterAll(types));
    {
        ObjectRegistry source(allocator, types, {0x53494E, 1});
        ObjectRegistry target(allocator, types, {0x53494E, 2});
        {
            RootRef<InheritHolder> holder = NewHolder(source);
            WithHolder(source, holder.Handle(), [](InheritHolder& value) noexcept {
                // 두 InheritMarker 하위 객체는 주소가 달라야 하므로 base 하위 객체가 앞으로 밀린다.
                CHECK(static_cast<const void*>(static_cast<const InheritBase*>(&value.offset)) !=
                      static_cast<const void*>(&value.offset));
            });
            FillHolder(source, holder.Handle());
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            CheckHolder(target, loaded.Value().Handle());
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

// base 멤버 경로(`leaf.a`·`offset.a` — 오프셋이 0이 아닌 base)와 InstancedStruct<InheritBase>의
// 파생 형 바꾸기 뒤 base·파생 멤버 쓰기가 실제 값에 닿는다. 무관한 struct로의 형 바꾸기는
// InvalidArgument(InvalidValue)이고 값은 그대로다.
TEST(StructInheritancePath_BaseMembersAndInstancedBase) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterAll(types));
    {
        ObjectRegistry objects(allocator, types, {0x53494E, 3});
        RootRef<InheritHolder> holder = NewHolder(objects);
        const ObjectHandle handle = holder.Handle();
        const StringView texts[] = {"leaf.a", "offset.a", "offset.d"};
        for (usize index = 0; index < 3; ++index) {
            PropertyPathBuffer path;
            REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, texts[index], path));
            REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, path.View(),
                              I32(allocator, static_cast<int64>(20 + index))));
        }

        WithHolder(objects, handle, [](InheritHolder& value) noexcept {
            CHECK(value.leaf.a == 20);
            CHECK(value.offset.a == 21 && value.offset.d == 22);
        });

        const PathStep shapeStep[] = {{FieldOf<InheritHolder>("shape")}};
        const PropertyPath shape{shapeStep};
        REQUIRE(PathSetInstancedType(objects, objects.OwnerAccess(), handle, shape,
                                     TypeIdOf<InheritOffset>()));
        PropertyPathBuffer baseMember;
        REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "shape.a", baseMember));
        REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, baseMember.View(),
                          I32(allocator, 31)));
        PropertyPathBuffer ownMember;
        REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "shape.d", ownMember));
        REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, ownMember.View(),
                          I32(allocator, 32)));
        WithHolder(objects, handle, [](InheritHolder& value) noexcept {
            const InheritOffset* offset = value.shape.Get<InheritOffset>();
            REQUIRE(offset);
            CHECK(offset->a == 31 && offset->d == 32);
        });

        PropertyPathFailure failure;
        auto other = PathSetInstancedType(objects, objects.OwnerAccess(), handle, shape,
                                          TypeIdOf<InheritOther>(), &failure);
        CHECK(!other && other.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidValue);
        WithHolder(objects, handle, [](InheritHolder& value) noexcept {
            CHECK(value.shape.Get<InheritOffset>() && value.shape.Get<InheritOffset>()->d == 32);
        });

        // lease 판 Emplace도 무관한 struct를 거부한다(값은 그대로).
        auto lease = types.AcquireValueType(TypeIdOf<InheritOther>(), TypeKind::Struct);
        REQUIRE(lease);
        WithHolder(objects, handle, [&](InheritHolder& value) noexcept {
            auto made = value.shape.Emplace(Move(lease.Value()), objects.ObjectAllocator());
            CHECK(!made && made.Error() == ErrorCode::InvalidArgument);
            CHECK(value.shape.Get<InheritOffset>());
        });

        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// decode: InstancedStruct<InheritBase> 자리의 파생 Record(base 소유 필드 ID 포함)는 받고, 무관한
// struct Record는 InvalidArgument다.
TEST(StructInheritanceGraph_InstancedBaseDecode) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterAll(types));
    {
        ObjectRegistry objects(allocator, types, {0x53494E, 4});
        auto seven = I64Payload(allocator, 7);
        const FieldBytes middleFields[] = {
            {FieldOf<InheritBase>("a"), static_cast<uint32>(WireType::I64), seven.View()}};
        auto middle = RecordOf(allocator, TypeIdOf<InheritMiddle>(),
                               TypeOf<InheritMiddle>().version, middleFields);
        auto accepted = ShapeGraph(allocator, middle.View());
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), accepted.View());
        REQUIRE(loaded);
        WithHolder(objects, loaded.Value().Handle(), [](InheritHolder& value) noexcept {
            const InheritMiddle* shape = value.shape.Get<InheritMiddle>();
            REQUIRE(shape);
            CHECK(shape->a == 7 && shape->b == 0u);
        });
        loaded.Value().Reset();

        const FieldBytes otherFields[] = {
            {FieldOf<InheritOther>("q"), static_cast<uint32>(WireType::I64), seven.View()}};
        auto other = RecordOf(allocator, TypeIdOf<InheritOther>(), TypeOf<InheritOther>().version,
                              otherFields);
        auto rejected = ShapeGraph(allocator, other.View());
        auto failed =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), rejected.View());
        CHECK(!failed && failed.Error() == ErrorCode::CorruptData);
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 파생 struct의 깊은 복사(TryCopyValue·InstancedStruct TryClone)는 base의 NATIVE 멤버와 상속 필드를
// 보존한다(B9j–B9m 묶음 리뷰 I1). base의 private 리플렉션 멤버도 생성 코드가 다룬다(리뷰 S4). 등록
// 형질은 상속 InstancedStruct 담기와 깊이를 반영한다.
TEST(StructInheritance_DeepCopyKeepsBaseMembers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterAll(types));
    {
        InheritLeaf source(allocator);
        source.a = 3;
        REQUIRE(source.name.TryAssign("name"));
        source.nativeCount = 7;
        REQUIRE(source.note.TryAssign("note"));
        source.b = 4;
        source.c = 1.5f;
        InheritLeaf copy(allocator);
        REQUIRE(detail::TryCopyValue(copy, source, allocator));
        CHECK(copy.a == 3 && copy.name.View() == "name" && copy.b == 4u && copy.c == 1.5f);
        CHECK(copy.nativeCount == 7u && copy.note.View() == "note");

        InstancedStruct<InheritBase> held;
        REQUIRE(held.Emplace<InheritLeaf>(types, allocator));
        held.Get<InheritLeaf>()->nativeCount = 9;
        REQUIRE(held.Get<InheritLeaf>()->note.TryAssign("held"));
        auto cloned = held.TryClone(allocator);
        REQUIRE(cloned);
        REQUIRE(cloned.Value().Get<InheritLeaf>());
        CHECK(cloned.Value().Get<InheritLeaf>()->nativeCount == 9u);
        CHECK(cloned.Value().Get<InheritLeaf>()->note.View() == "held");

        InheritPrivateLeaf hidden;
        hidden.SetSecret(5);
        hidden.open = 6;
        InheritPrivateLeaf hiddenCopy;
        REQUIRE(detail::TryCopyValue(hiddenCopy, hidden, allocator));
        CHECK(hiddenCopy.Secret() == 5 && hiddenCopy.open == 6);

        auto shape = types.AcquireValueType(TypeIdOf<InheritShapeChild>(), TypeKind::Struct);
        REQUIRE(shape);
        CHECK(shape.Value().Struct().ContainsInstancedStruct());
        auto nested = types.AcquireValueType(TypeIdOf<InheritNestedLeaf>(), TypeKind::Struct);
        REQUIRE(nested);
        CHECK(nested.Value().Struct().Depth() == 2u);
    }

    CHECK(types.Shutdown());
}
} // namespace struct_inheritance_tests_file
