#include <Base/Compiler.h>
#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/NativeMemberTraits.h>
#include <TestSupport/Test.h>

// B2 지정자가 생성 스키마와 TypeRegistry에 닿는지, NATIVE 멤버 형질이 맞는지
// 본다(Docs/Design/ReflectionFoundations.ko.md 2절).
using namespace rocks;
namespace specifier_tests_file {

using reflection_fixture::Node;
using reflection_fixture::WorldChild;
using reflection_fixture::WorldFixture;

namespace {

struct Lease {
    static constexpr bool RocksRequiresWorldOnlyOwner = true;
};

struct Opaque;

const FieldSchema* FindField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return &field;
        }
    }

    return nullptr;
}

} // namespace

// 생성 코드의 static_assert가 쓰는 형질이다.
static_assert(IsNativeStrongReference<ObjectRef<Node>>);
static_assert(IsNativeStrongReference<RootRef<Node>[2]>);
static_assert(IsNativeStrongReference<ObjectRefArray<Node>>);
static_assert(IsNativeStrongReference<ObjectRef<Node>*>);
static_assert(IsNativeStrongReference<const RootRef<Node>&>);
static_assert(IsNativeStrongReference<ObjectRef<Node>**>);
// Object 파생 타입을 가리키는 raw 포인터·참조는 형질이 아니라 MetaCompiler 탐침(RMC2205)이
// 거부한다(대상의 완전성에 기대는 판정은 TU마다 달라질 수 있는 ODR 위반이다).
static_assert(!IsNativeStrongReference<Object*>);
static_assert(!IsNativeStrongReference<const Node*>);
static_assert(!IsNativeStrongReference<Node&>);
static_assert(!IsNativeStrongReference<Node**>);
static_assert(!IsNativeStrongReference<WeakObjectRef<Node>>);
static_assert(!IsNativeStrongReference<int32*>);
static_assert(!IsNativeStrongReference<Opaque*>);
static_assert(!IsNativeStrongReference<int32>);
static_assert(RequiresWorldOnlyOwner<Lease>);
static_assert(RequiresWorldOnlyOwner<const Lease[3]>);
static_assert(!RequiresWorldOnlyOwner<int32>);

TEST(ReflectionSpecifiers_VersionFlagsAndToolNamesReachTheSchema) {
    const TypeSchema& world = TypeOf<WorldFixture>();
    CHECK(world.version == 2);
    CHECK(HasFlag(world.flags, TypeFlags::WorldOnly));
    // NATIVE 멤버(runtimeCache)는 스키마 필드가 아니다. EditorOnly 필드(note)는 에디터 데이터
    // 조합에서만 스키마 필드이고, 두 조합 모두 EditorOnly 필드 ID 목록에
    // 있다(Docs/Design/BuildTargets 3.4절 BS2c).
    REQUIRE(world.editorOnlyFieldIds.Size() == 1);
#if ROCKS_WITH_EDITOR_DATA
    REQUIRE(world.fields.Size() == 2);
    const FieldSchema* note = FindField(world, "note");
    REQUIRE(note);
    CHECK(HasFlag(note->flags, FieldFlags::EditorOnly));
    CHECK(note->id == world.editorOnlyFieldIds[0]);
    CHECK(StringView(note->toolName.Data(), note->toolName.Size()) == StringView("Label"));
#else
    REQUIRE(world.fields.Size() == 1);
    CHECK(FindField(world, "note") == nullptr &&
          IsEditorOnlyFieldId(world, world.editorOnlyFieldIds[0]));
#endif
    const FieldSchema* target = FindField(world, "target");
    REQUIRE(target);
    CHECK(HasFlag(target->flags, FieldFlags::Required));
    CHECK(target->kind == FieldKind::ObjectReference);
    CHECK(target->toolName.Size() == 0);

    // WorldOnly는 파생에 상속되고, 버전은 타입마다 따로다.
    const TypeSchema& child = TypeOf<WorldChild>();
    CHECK(child.version == 1);
    CHECK(HasFlag(child.flags, TypeFlags::WorldOnly));
    // 상속한 EditorOnly 필드 ID도 목록에 있다.
    CHECK(child.fields.Size() == (ROCKS_WITH_EDITOR_DATA ? 3u : 2u) &&
          child.editorOnlyFieldIds.Size() == 1);
}

TEST(ReflectionSpecifiers_RegistryAcceptsGeneratedFlags) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto world = GeneratedType<WorldFixture>::Register(types);
    REQUIRE(world);
    auto child = GeneratedType<WorldChild>::Register(types);
    REQUIRE(child);
    auto lease = types.Acquire(TypeIdOf<WorldChild>());
    REQUIRE(lease);
    CHECK(lease.Value().Schema().version == 1);
    CHECK(HasFlag(lease.Value().Schema().flags, TypeFlags::WorldOnly));
    lease.Value().Reset();
    REQUIRE(types.Unregister(child.Value()));
    REQUIRE(types.Unregister(world.Value()));
    CHECK(types.Shutdown());
}
} // namespace specifier_tests_file
