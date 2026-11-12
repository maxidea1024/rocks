#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Localization/Text.h>

using namespace rocks;

namespace {
class StoreReentryAllocator final : public Allocator {
public:
    explicit StoreReentryAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (armed) {
            // 단 한 번만 재진입한다. 중첩 교체의 cleanup이 다시 이 콜백을 호출해도 순환하지 않는다.
            armed = false;
            ++callbackCount;
            auto acquired = store->Acquire();
            observedReplacement = acquired && acquired.Value().Revision() == 2;
            auto published = store->Publish(*third);
            publishedThird = published.IsOk();
        }
        return backing_.Deallocate(address);
    }

    CatalogStore* store = nullptr;
    const CatalogSnapshot* third = nullptr;
    bool armed = false;
    bool observedReplacement = false;
    bool publishedThird = false;
    uint32 callbackCount = 0;

private:
    Allocator& backing_;
};
} // namespace

TEST(CatalogOldSnapshotCleanupCanReenterAcquireAndPublishOutsideStoreGate) {
    SystemAllocator backing;
    StoreReentryAllocator allocator(backing);
    {
        CatalogStore store;
        const CatalogEntry entries[] = {{{1, 1}, 1, "text"}};
        auto first = CatalogSnapshot::Create(allocator, "first", 1, {entries, 1});
        auto second = CatalogSnapshot::Create(allocator, "second", 2, {entries, 1});
        auto third = CatalogSnapshot::Create(allocator, "third", 3, {entries, 1});
        REQUIRE(first && second && third);
        REQUIRE(store.Publish(first.Value()));
        first.Value().Reset();
        allocator.store = &store;
        allocator.third = &third.Value();
        allocator.armed = true;
        REQUIRE(store.Publish(second.Value()));
        CHECK(!allocator.armed);
        CHECK(allocator.callbackCount == 1);
        CHECK(allocator.observedReplacement);
        CHECK(allocator.publishedThird);
        auto current = store.Acquire();
        REQUIRE(current);
        CHECK(current.Value().Revision() == 3);
        CHECK(current.Value().Culture() == StringView("third"));
        store.Close();
    }
    CHECK(backing.Stats().liveAllocations == 0);
}
