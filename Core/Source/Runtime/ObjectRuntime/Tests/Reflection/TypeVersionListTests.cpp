#include <ReflectionTestFixtures/ArrayFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// TypeRegistry 타입 목록 시험이다(Docs/Design/AssetSystem 10.2절 "A4c 구현 세부" 기반 보강
// 1): 빈 Registry, 종류마다(클래스·struct·enum — enum 버전 0) TypeId 순, 해제 뒤 빠짐, 다른 스레드
// WrongThread, Shutdown 뒤 ShuttingDown, 할당 실패(out은 빈다), out을 비우고 채움.
using namespace rocks;
using namespace rocks::reflection_fixture;

namespace type_version_list_tests_file {

namespace {

bool Sorted(const Array<TypeVersionEntry>& list) noexcept {
    for (usize index = 1; index < list.Size(); ++index) {
        if (!GuidLess(list[index - 1].type, list[index].type)) {
            return false;
        }
    }

    return true;
}

const TypeVersionEntry* Find(const Array<TypeVersionEntry>& list, TypeId type) noexcept {
    for (const TypeVersionEntry& entry : list) {
        if (entry.type == type) {
            return &entry;
        }
    }

    return nullptr;
}

ErrorCode ListOnWorker(const TypeRegistry& types, Array<TypeVersionEntry>& out) noexcept {
    struct Foreign {
        const TypeRegistry* types;
        Array<TypeVersionEntry>* out;
        ErrorCode error = ErrorCode::None;
    } foreign{&types, &out};
    platform::Thread thread;
    auto entry = [](void* context) noexcept {
        auto* state = static_cast<Foreign*>(context);
        auto listed = state->types->ListTypeVersions(*state->out);
        state->error = listed ? ErrorCode::None : listed.Error();
    };
    if (thread.Start(entry, &foreign) != platform::ThreadStatus::Success) {
        return ErrorCode::DependencyFailed;
    }

    if (thread.Join() != platform::ThreadStatus::Success) {
        return ErrorCode::DependencyFailed;
    }

    return foreign.error;
}

} // namespace

// 목록: 빈 Registry는 빈 목록, 클래스·struct·enum이 TypeId 순으로 종류·스키마 버전과 함께 나온다
// (enum은 0). 해제한 타입은 빠지고 out은 비우고 채운다.
TEST(TypeRegistry_ListTypeVersions) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    Array<TypeVersionEntry> list(allocator);
    REQUIRE(list.TryPushBack(TypeVersionEntry{Guid{9, 9}, TypeKind::Struct, 3}));
    REQUIRE(types.ListTypeVersions(list));
    CHECK(list.IsEmpty());

    auto group = GeneratedType<TextureGroup>::Register(types);
    auto bias = GeneratedType<Bias>::Register(types);
    auto channels = GeneratedType<Channels>::Register(types);
    auto owner = GeneratedType<EnumFields>::Register(types);
    auto tag = GeneratedType<ArrayTag>::Register(types);
    REQUIRE(group && bias && channels && owner && tag);
    REQUIRE(types.ListTypeVersions(list));
    REQUIRE(list.Size() == 5);
    CHECK(Sorted(list));
    const TypeVersionEntry* enumEntry = Find(list, TypeIdOf<TextureGroup>());
    const TypeVersionEntry* classEntry = Find(list, TypeIdOf<EnumFields>());
    const TypeVersionEntry* structEntry = Find(list, TypeIdOf<ArrayTag>());
    REQUIRE(enumEntry != nullptr && classEntry != nullptr && structEntry != nullptr);
    CHECK(enumEntry->kind == TypeKind::Enum && enumEntry->version == 0);
    CHECK(classEntry->kind == TypeKind::Class &&
          classEntry->version == GeneratedType<EnumFields>::Schema().version);
    CHECK(structEntry->kind == TypeKind::Struct &&
          structEntry->version == GeneratedType<ArrayTag>::Schema().version);

    REQUIRE(types.Unregister(tag.Value()));
    REQUIRE(types.ListTypeVersions(list));
    CHECK(list.Size() == 4 && Find(list, TypeIdOf<ArrayTag>()) == nullptr && Sorted(list));
    CHECK(types.Shutdown());
}

// 거부: 다른 스레드 WrongThread, Shutdown 뒤 ShuttingDown, 할당 실패 OutOfMemory — 셋 다 out은
// 빈다.
TEST(TypeRegistry_ListTypeVersionsRejects) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(GeneratedType<TextureGroup>::Register(types));
    Array<TypeVersionEntry> list(allocator);
    REQUIRE(list.TryPushBack(TypeVersionEntry{}));
    CHECK(ListOnWorker(types, list) == ErrorCode::WrongThread);
    CHECK(list.IsEmpty());

    test::NFailAllocator failing(allocator);
    Array<TypeVersionEntry> failed(failing);
    failing.FailOnAttempt(1);
    CHECK(types.ListTypeVersions(failed).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(failed.IsEmpty());
    REQUIRE(types.ListTypeVersions(failed));
    CHECK(failed.Size() == 1);

    REQUIRE(types.Shutdown());
    REQUIRE(list.TryPushBack(TypeVersionEntry{}));
    CHECK(types.ListTypeVersions(list).Error() == ErrorCode::ShuttingDown);
    CHECK(list.IsEmpty());
}

} // namespace type_version_list_tests_file
