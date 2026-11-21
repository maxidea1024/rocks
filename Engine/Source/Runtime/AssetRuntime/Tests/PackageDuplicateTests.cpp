#include "AssetTestSupport.h"

#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageWriter.h>
#include <AssetTestFixtures/TestAssets.h>

// writer 복제 옵션 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 17 A2e3b — PackageWriteOptions::
// duplicateId): 헤더 id는 사본 id, DuplicateTransient·NonTryoutDuplicateTransient 필드는(struct
// 필드·원소 배열 안의 멤버도) 쓰지 않아 로드가 클래스 기본값, 자기 패키지를 가리키는 Soft 참조는
// 사본 id(자기 의존이 아니다), 다른 Soft 참조는 그대로. 복제가 아닌 쓰기는 모두 그대로다. 쿠킹·지금
// id와 같은 사본 id는 InvalidArgument.
using namespace rocks;
namespace package_duplicate_tests_file {

using namespace rocks::asset_test;
using asset_fixture::DuplicableAsset;

namespace {

constexpr uint64 SourceId = 0xd00;
constexpr uint64 CopyId = 0xd01;
constexpr uint64 OtherId = 0xd02;
constexpr int32 Kept = 11;
constexpr int32 Cache = 22;
constexpr int32 Instance = 33;

// bytes를 env.target에 로드해 대표 에셋 값을 준다(실패하면 null).
const DuplicableAsset* LoadAsset(AssetEnv& env, const Array<uint8>& bytes, RootRef<Object>& root,
                                 ObjectHandle& handle) noexcept {
    Array<uint8> files[1] = {Copy(env.allocator, bytes.View())};
    if (!LoadAll(env, *env.target, Span<Array<uint8>>(files, 1), PackageKind::Editor, nullptr,
                 Span<RootRef<Object>>(&root, 1), Span<ObjectHandle>(&handle, 1))) {
        return nullptr;
    }

    auto representative = env.target->FindChild(env.target->OwnerAccess(), handle, "");
    return representative ? Pointer<DuplicableAsset>(*env.target, representative.Value()) : nullptr;
}

} // namespace

TEST(AssetPackage_DuplicateWrite) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    auto made =
        NewPackage(registry, registry.OwnerAccess(), FixedId(SourceId), PackageKind::Editor);
    REQUIRE(made);
    RootRef<Package> package = Move(made.Value());
    const ObjectHandle asset =
        Make<DuplicableAsset>(registry, GeneratedType<DuplicableAsset>::Id(), package.Handle(), "");
    DuplicableAsset* value = Pointer<DuplicableAsset>(registry, asset);
    REQUIRE(value != nullptr);
    value->kept = Kept;
    value->cache = Cache;
    value->instance = Instance;
    REQUIRE(value->self.TryAssign(FixedId(SourceId), ""));
    REQUIRE(value->other.TryAssign(FixedId(OtherId), ""));
    // struct 필드·원소 배열 안의 제외 멤버.
    value->slot.kept = Kept;
    value->slot.cache = Cache;
    const asset_fixture::DuplicableSlot element{Kept, Cache};
    REQUIRE(value->slots.TryPushBack(element) && value->slots.TryPushBack(element));

    // 복제가 아닌 쓰기: 자기 Soft 참조는 의존이 아니고 값은 모두 그대로다.
    const Array<uint8> plain = Write(env.allocator, registry, package.Handle(), EditorOptions());
    REQUIRE(!plain.IsEmpty());
    auto plainRead =
        ReadPackage(env.allocator, plain.View(), LoadOptions(PackageKind::Editor).read);
    REQUIRE(plainRead);
    CHECK(plainRead.Value().header.assetId == FixedId(SourceId));
    REQUIRE(plainRead.Value().dependencies.Size() == 1);
    CHECK(plainRead.Value().dependencies[0].asset == FixedId(OtherId) &&
          plainRead.Value().dependencies[0].kind == PackageDependencyKind::Soft);

    // 복제 쓰기.
    PackageWriteOptions duplicate = EditorOptions();
    duplicate.duplicateId = FixedId(CopyId);
    const Array<uint8> copied = Write(env.allocator, registry, package.Handle(), duplicate);
    REQUIRE(!copied.IsEmpty());
    auto copiedRead =
        ReadPackage(env.allocator, copied.View(), LoadOptions(PackageKind::Editor).read);
    REQUIRE(copiedRead);
    CHECK(copiedRead.Value().header.assetId == FixedId(CopyId));
    REQUIRE(copiedRead.Value().dependencies.Size() == 1);
    CHECK(copiedRead.Value().dependencies[0].asset == FixedId(OtherId));
    // 원본 객체는 바뀌지 않는다(저장은 객체를 바꾸지 않는다).
    CHECK(value->cache == Cache && value->self.asset == FixedId(SourceId));

    RootRef<Object> root;
    ObjectHandle handle;
    const DuplicableAsset* loaded = LoadAsset(env, copied, root, handle);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->kept == Kept && loaded->cache == 0 && loaded->instance == 0);
    CHECK(loaded->self.asset == FixedId(CopyId) && loaded->self.objectPath.View().IsEmpty());
    CHECK(loaded->other.asset == FixedId(OtherId));
    CHECK(loaded->slot.kept == Kept && loaded->slot.cache == 0);
    REQUIRE(loaded->slots.Size() == 2);
    CHECK(loaded->slots[0].kept == Kept && loaded->slots[0].cache == 0 &&
          loaded->slots[1].kept == Kept && loaded->slots[1].cache == 0);

    // 거부: 지금 id와 같은 사본 id, 쿠킹 패키지의 복제.
    AssetDiagnostic failure;
    MemoryPackageSink sink(env.allocator);
    PackageWriteOptions same = EditorOptions();
    same.duplicateId = FixedId(SourceId);
    CHECK(WritePackage(env.allocator, registry, registry.OwnerAccess(), package.Handle(), same,
                       sink, &failure)
              .Error() == ErrorCode::InvalidArgument);
    PackageWriteOptions cooked = CookedOptions();
    cooked.duplicateId = FixedId(CopyId);
    MemoryPackageSink cookedSink(env.allocator);
    CHECK(WritePackage(env.allocator, registry, registry.OwnerAccess(), package.Handle(), cooked,
                       cookedSink, &failure)
              .Error() == ErrorCode::InvalidArgument);
}

} // namespace package_duplicate_tests_file
