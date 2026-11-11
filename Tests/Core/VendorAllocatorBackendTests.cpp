#include <Core/Memory/ExternalAllocator.h>
#include <Core/Memory/VendorAllocatorBackend.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

struct VendorFixture {
    alignas(256) unsigned char storage[256]{};
    usize first = 0;
    usize second = 0;
    uint32 allocations = 0;
    uint32 releases = 0;
    void* releasedAddress = nullptr;
    bool fail = false;
};

VendorFixture fixture;

// 실제 라이브러리의 ABI 연결만 모의한다. 외부 공급자 자체의 검증은 아니다.
extern "C" void* AllocateForVendorTest(usize first, usize second) noexcept {
    fixture.first = first;
    fixture.second = second;
    ++fixture.allocations;
    return fixture.fail ? nullptr : fixture.storage;
}

extern "C" void FreeForVendorTest(void* address) noexcept {
    fixture.releasedAddress = address;
    ++fixture.releases;
}

struct ModuleCounts {
    uint32 retain = 0;
    uint32 release = 0;
};

void RetainForVendorTest(void* context) noexcept {
    ++static_cast<ModuleCounts*>(context)->retain;
}

void ReleaseForVendorTest(void* context) noexcept {
    ++static_cast<ModuleCounts*>(context)->release;
}

void ResetFixture() noexcept {
    fixture.first = 0;
    fixture.second = 0;
    fixture.allocations = 0;
    fixture.releases = 0;
    fixture.releasedAddress = nullptr;
    fixture.fail = false;
}

template <typename T>
concept TemporaryBindAllowed = requires(T&& value) { static_cast<T&&>(value).Bind(); };

// callback context는 provider를 빌리므로 임시 객체에서는 Bind할 수 없다.
static_assert(!TemporaryBindAllowed<JemallocBackend>);
static_assert(!TemporaryBindAllowed<TcmallocBackend>);
static_assert(!TemporaryBindAllowed<MimallocBackend>);

template <typename Provider>
void CheckAlignedFirstProvider() noexcept {
    ResetFixture();
    Provider provider(AllocateForVendorTest, FreeForVendorTest);
    auto bound = provider.Bind();
    REQUIRE(bound);
    auto& backend = bound.Value();

    auto zero = backend.allocate(backend.context, 0, 16, 0);
    REQUIRE(zero);
    CHECK(zero.Value() == nullptr);
    CHECK(fixture.allocations == 0);
    CHECK(backend.allocate(backend.context, 0, 3, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(backend.allocate(backend.context, MaxSize, 16, 0).Error() == ErrorCode::Overflow);
    CHECK(fixture.allocations == 0);

    auto block = backend.allocate(backend.context, 17, 16, 123);
    REQUIRE(block);
    CHECK(block.Value() == fixture.storage);
    CHECK(fixture.first == 16);
    CHECK(fixture.second == 32);
    CHECK(fixture.allocations == 1);
    REQUIRE(backend.deallocate(backend.context, block.Value()));
    CHECK(fixture.releases == 1);
    CHECK(fixture.releasedAddress == fixture.storage);

    // 이미 정렬 배수인 크기는 불필요하게 커지지 않는다.
    auto exact = backend.allocate(backend.context, 32, 16, 0);
    REQUIRE(exact);
    CHECK(fixture.first == 16 && fixture.second == 32);
    REQUIRE(backend.deallocate(backend.context, exact.Value()));
    CHECK(fixture.releases == 2);

    auto smallest = backend.allocate(backend.context, 3, 1, 0);
    REQUIRE(smallest);
    CHECK(fixture.first == 1 && fixture.second == 3);
    REQUIRE(backend.deallocate(backend.context, smallest.Value()));
    CHECK(fixture.releases == 3);

    fixture.fail = true;
    CHECK(backend.allocate(backend.context, 17, 16, 0).Error() == ErrorCode::OutOfMemory);
    CHECK(fixture.allocations == 4);
    CHECK(backend.allocate(nullptr, 17, 16, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(backend.deallocate(nullptr, fixture.storage).Error() == ErrorCode::InvalidArgument);
}

} // namespace

TEST(CoreVendorAlignedFirstAbiAndFailurePaths) {
    CheckAlignedFirstProvider<JemallocBackend>();
    CheckAlignedFirstProvider<TcmallocBackend>();

    JemallocBackend missingAllocate(nullptr, FreeForVendorTest);
    JemallocBackend missingFree(AllocateForVendorTest, nullptr);
    TcmallocBackend missingBoth(nullptr, nullptr);
    CHECK(missingAllocate.Bind().Error() == ErrorCode::InvalidArgument);
    CHECK(missingFree.Bind().Error() == ErrorCode::InvalidArgument);
    CHECK(missingBoth.Bind().Error() == ErrorCode::InvalidArgument);
}

TEST(CoreVendorMimallocAbiAndFailurePaths) {
    ResetFixture();
    MimallocBackend provider(AllocateForVendorTest, FreeForVendorTest);
    auto bound = provider.Bind();
    REQUIRE(bound);
    auto& backend = bound.Value();

    auto zero = backend.allocate(backend.context, 0, 16, 0);
    REQUIRE(zero);
    CHECK(zero.Value() == nullptr);
    CHECK(fixture.allocations == 0);
    CHECK(backend.allocate(backend.context, 8, 3, 0).Error() == ErrorCode::InvalidArgument);

    auto block = backend.allocate(backend.context, 17, 16, 55);
    REQUIRE(block);
    CHECK(block.Value() == fixture.storage);
    CHECK(fixture.first == 17);
    CHECK(fixture.second == 16);
    REQUIRE(backend.deallocate(backend.context, block.Value()));
    CHECK(fixture.releases == 1);
    CHECK(fixture.releasedAddress == fixture.storage);

    fixture.fail = true;
    CHECK(backend.allocate(backend.context, 17, 16, 0).Error() == ErrorCode::OutOfMemory);
    CHECK(backend.allocate(nullptr, 17, 16, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(backend.deallocate(nullptr, fixture.storage).Error() == ErrorCode::InvalidArgument);

    MimallocBackend missing(nullptr, FreeForVendorTest);
    CHECK(missing.Bind().Error() == ErrorCode::InvalidArgument);
}

TEST(CoreVendorBindingRetainsModuleAndExternalOwnership) {
    ResetFixture();
    ModuleCounts counts;
    auto module = ModuleLease::Acquire(&counts, RetainForVendorTest, ReleaseForVendorTest);
    REQUIRE(module);
    CHECK(counts.retain == 1);

    MimallocBackend provider(AllocateForVendorTest, FreeForVendorTest);
    {
        auto bound = provider.Bind(module.Value());
        REQUIRE(bound);
        CHECK(counts.retain == 2);
        ExternalAllocator allocator(Move(bound.Value()));
        module.Value().Reset();
        CHECK(counts.release == 1);

        CHECK(allocator.Allocate(MaxSize, 16).Error() == ErrorCode::Overflow);
        CHECK(fixture.allocations == 0);
        auto block = allocator.Allocate(17, 16, 123);
        REQUIRE(block);
        CHECK(block.Value() == fixture.storage);
        CHECK(fixture.first == 17 && fixture.second == 16);
        CHECK(allocator.Stats().liveAllocations == 1);
        CHECK(allocator.Stats().liveBytes == 17);
        int32 foreign = 0;
        CHECK(allocator.Deallocate(&foreign).Error() == ErrorCode::NotFound);
        REQUIRE(allocator.Deallocate(block.Value()));
        CHECK(allocator.Deallocate(block.Value()).Error() == ErrorCode::NotFound);
        CHECK(fixture.releases == 1);
        CHECK(counts.release == 1);
    }
    CHECK(counts.release == 2);
}
