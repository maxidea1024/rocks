#include "AssetTestSupport.h"

#include <AssetRuntime/MemoryBulkSource.h>
#include <TestSupport/NFailAllocator.h>

// MemoryBulkSource 시험이다(Docs/Design/MeshAnimationAssets 3.7절 2 — 저장 전 bulk raw byte의
// 메모리 원본): 임시 bulkId 발급·크기·SHA-256, 0 byte entry, 없는 ref, 할당 실패 시 무변경, 그리고
// WritePackage가 그 ref로 읽어 패키지에 쓰는지.
using namespace rocks;
namespace memory_bulk_source_tests_file {

using namespace rocks::asset_test;

TEST(AssetPackage_MemoryBulkSourceEntries) {
    SystemAllocator allocator;
    MemoryBulkSource bulk(allocator);
    CHECK(bulk.Count() == 0);
    const uint8 data[] = {1, 2, 3, 4, 5};
    auto first = bulk.AddCopy(Span<const uint8>(data, 5));
    REQUIRE(first);
    CHECK(first.Value().bulkId == 1 && first.Value().rawBytes == 5);
    CHECK(first.Value().sha256 == Sha256::Hash(Span<const uint8>(data, 5)));
    Array<uint8> empty(allocator);
    auto second = bulk.Add(Move(empty));
    REQUIRE(second);
    CHECK(second.Value().bulkId == 2 && second.Value().rawBytes == 0 && !second.Value().IsEmpty());
    CHECK(bulk.Count() == 2);
    auto read = bulk.ReadBulk(first.Value());
    REQUIRE(read);
    CHECK(SameBytes(read.Value(), Span<const uint8>(data, 5)));
    CHECK(bulk.ReadBulk(second.Value()).Value().IsEmpty());
    // 빈 ref와 없는 번호는 NotFound다.
    CHECK(bulk.ReadBulk(BulkDataRef{}).Error() == ErrorCode::NotFound);
    BulkDataRef missing = first.Value();
    missing.bulkId = 3;
    CHECK(bulk.ReadBulk(missing).Error() == ErrorCode::NotFound);
    bulk.Clear();
    CHECK(bulk.Count() == 0 && bulk.ReadBulk(first.Value()).Error() == ErrorCode::NotFound);
}

// 할당 실패: Add의 칸 확보·AddCopy의 복사가 실패하면 그 오류이고 entry가 늘지 않으며, Add에 넘긴
// byte는 호출자에게 남는다.
TEST(AssetPackage_MemoryBulkSourceFailures) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    MemoryBulkSource bulk(failing);
    Array<uint8> bytes(allocator);
    REQUIRE(bytes.TryResize(16));
    failing.FailOnAttempt(1);
    auto added = bulk.Add(Move(bytes));
    CHECK(!added && added.Error() == ErrorCode::OutOfMemory);
    CHECK(bulk.Count() == 0 && bytes.Size() == 16);
    const uint8 data[] = {9, 8, 7};
    failing.FailOnAttempt(1);
    auto copied = bulk.AddCopy(Span<const uint8>(data, 3));
    CHECK(!copied && copied.Error() == ErrorCode::OutOfMemory);
    CHECK(bulk.Count() == 0);
    failing.DisableFailure();
    CHECK(bulk.AddCopy(Span<const uint8>(data, 3)));
}

// WritePackage가 MemoryBulkSource의 ref로 byte를 읽어 쓰고, 로드한 패키지의 bulk가 같은 byte다.
TEST(AssetPackage_MemoryBulkSourceWrites) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(4100), PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "");
    TestAsset* pointer = Pointer<TestAsset>(source, asset);
    REQUIRE(pointer != nullptr);
    MemoryBulkSource bulk(env.allocator);
    const uint8 data[] = {0xde, 0xad, 0xbe, 0xef};
    REQUIRE(bulk.AddCopy(Span<const uint8>(data, 2)));
    auto ref = bulk.AddCopy(Span<const uint8>(data, 4));
    REQUIRE(ref);
    pointer->pixels = ref.Value();
    PackageWriteOptions options = CookedOptions();
    options.bulkSource = &bulk;
    Array<uint8> file = Write(env.allocator, source, package.Value().Handle(), options);
    REQUIRE(!file.IsEmpty());
    Array<uint8> files[1] = {Move(file)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    auto loaded = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    REQUIRE(loaded);
    TestAsset* read = Pointer<TestAsset>(*env.target, loaded.Value());
    Package* readPackage = Pointer<Package>(*env.target, handles[0]);
    REQUIRE(read != nullptr && readPackage != nullptr);
    // 저장은 번호를 1부터 다시 매긴다(쓰인 entry는 하나).
    CHECK(read->pixels.bulkId == 1 && readPackage->BulkCount() == 1);
    auto bytes = readPackage->BulkBytes(read->pixels);
    REQUIRE(bytes);
    CHECK(SameBytes(bytes.Value(), Span<const uint8>(data, 4)));
}

} // namespace memory_bulk_source_tests_file
