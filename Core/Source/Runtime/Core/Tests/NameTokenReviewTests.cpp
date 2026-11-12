#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

struct MixedNameWorker {
    NamePool* pool = nullptr;
    platform::ManualResetEvent* start = nullptr;
    StringView spelling;
    Name result{};
    bool readCorrect = false;
};

void InternMixedName(void* data) noexcept {
    auto& worker = *static_cast<MixedNameWorker*>(data);
    if (worker.start->Wait(10000) != platform::WaitResult::Signaled) {
        return;
    }

    auto name = worker.pool->TryIntern(worker.spelling);
    if (!name) {
        return;
    }

    worker.result = name.Value();
    auto lease = worker.pool->Acquire(worker.result);
    worker.readCorrect = lease && lease.Value().View() == worker.spelling;
}

} // namespace

TEST(NameTokenRejectsForgedIdentityAndDisplayFields) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        auto canonical = pool.TryIntern("Alpha_7");
        auto sameNameOtherDisplay = pool.TryIntern("aLPHa_7");
        auto differentNumber = pool.TryIntern("Alpha_8");
        auto differentName = pool.TryIntern("Beta_7");
        REQUIRE(canonical && sameNameOtherDisplay && differentNumber && differentName);
        const Name original = canonical.Value();
        const Name alternate = sameNameOtherDisplay.Value();
        REQUIRE(original.IsValid() && alternate.IsValid());
        CHECK(original == alternate && original.displayId != alternate.displayId);
        CHECK(Hash<Name>{}(original) == Hash<Name>{}(alternate));

        Name broken = original;
        broken.displayId = 0;
        CHECK(!broken.IsValid());
        CHECK(broken != original);
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = original;
        broken.id = 0;
        CHECK(!broken.IsValid());
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = original;
        broken.domain = 0;
        CHECK(!broken.IsValid());
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = original;
        broken.number = 0;
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = original;
        broken.number = static_cast<uint32>(-1);
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = original;
        broken.displayId = differentNumber.Value().displayId;
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = original;
        broken.displayId = differentName.Value().displayId;
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = original;
        broken.id = differentName.Value().id;
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);

        // 같은 비교 ID와 번호에 속하는 다른 표시 ID는 실제 발급된 이름을 가리킨다.
        Name alternateDisplay = original;
        alternateDisplay.displayId = alternate.displayId;
        CHECK(alternateDisplay == alternate);
        auto alternateLease = pool.Acquire(alternateDisplay);
        REQUIRE(alternateLease);
        CHECK(alternateLease.Value().View() == StringView("aLPHa_7"));
        CHECK(alternateLease.Value().BaseView() == StringView("aLPHa"));
        auto clone = alternateLease.Value().TryClone();
        REQUIRE(clone);
        CHECK(clone.Value().Identifier().displayId == alternate.displayId);
        CHECK(clone.Value().View() == StringView("aLPHa_7"));

        HashSet<Name> set(allocator);
        REQUIRE(set.Add(original));
        Name incomplete = original;
        incomplete.displayId = 0;
        CHECK(!set.Contains(incomplete));
        CHECK(set.Contains(alternate));
        CHECK(!set.Contains(differentNumber.Value()));
        CHECK(!set.Contains(differentName.Value()));
        auto busy = pool.Shutdown();
        CHECK(!busy && busy.Error() == ErrorCode::Busy);
        auto lateClone = alternateLease.Value().TryClone();
        CHECK(!lateClone && lateClone.Error() == ErrorCode::ShuttingDown);
        clone.Value().Reset();
        alternateLease.Value().Reset();
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameTokenDomainRestartAndForeignPoolAreDistinct) {
    SystemAllocator allocator;
    {
        NamePool first(allocator);
        NamePool second(allocator);
        REQUIRE(first.Initialize());
        REQUIRE(second.Initialize());
        auto a = first.TryIntern("Actor_7");
        auto b = second.TryIntern("actor_7");
        REQUIRE(a && b);
        CHECK(a.Value().id == b.Value().id && a.Value().displayId == b.Value().displayId);
        CHECK(a.Value().domain != b.Value().domain);
        CHECK(a.Value() != b.Value());
        CHECK(first.Acquire(b.Value()).Error() == ErrorCode::StaleHandle);
        CHECK(second.Acquire(a.Value()).Error() == ErrorCode::StaleHandle);
        const Name stale = a.Value();
        REQUIRE(first.Shutdown());
        REQUIRE(first.Initialize());
        auto restarted = first.TryIntern("Actor_7");
        REQUIRE(restarted);
        CHECK(restarted.Value().domain != stale.domain);
        CHECK(restarted.Value().id == stale.id);
        CHECK(first.Acquire(stale).Error() == ErrorCode::StaleHandle);
        auto readable = first.Acquire(restarted.Value());
        REQUIRE(readable);
        CHECK(readable.Value().View() == StringView("Actor_7"));
        readable.Value().Reset();
        REQUIRE(first.Shutdown());
        REQUIRE(second.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameNumberSuffixAfterEmbeddedNulKeepsCompleteDisplay) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        constexpr char firstBytes[] = {'A', '\0', 'Z', '_', '7'};
        constexpr char secondBytes[] = {'a', '\0', 'z', '_', '7'};
        constexpr char baseBytes[] = {'a', '\0', 'z'};
        auto first = pool.TryIntern(StringView(firstBytes, sizeof(firstBytes)));
        auto second = pool.TryIntern(StringView(secondBytes, sizeof(secondBytes)));
        REQUIRE(first && second);
        CHECK(first.Value() == second.Value());
        CHECK(first.Value().GetNumber() == 7);
        auto lease = pool.Acquire(second.Value());
        REQUIRE(lease);
        CHECK(lease.Value().View() == StringView(secondBytes, sizeof(secondBytes)));
        CHECK(lease.Value().BaseView() == StringView(baseBytes, sizeof(baseBytes)));
        lease.Value().Reset();
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameForcedCollisionsWithConcurrentDifferentComparisonNames) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        NamePoolConfig config;
        config.hashMask = 0;
        REQUIRE(pool.Initialize(config));
        platform::ManualResetEvent start;
        const StringView spellings[] = {"Alpha_1", "ALPHA_1", "Alpha_2", "Beta_1",
                                        "BETA_1",  "Beta",    "Alpha",   "ALPHA_2"};
        MixedNameWorker workers[8]{};
        platform::Thread threads[8];
        usize started = 0;
        for (; started < 8; ++started) {
            workers[started].pool = &pool;
            workers[started].start = &start;
            workers[started].spelling = spellings[started];
            if (threads[started].Start(InternMixedName, &workers[started]) !=
                platform::ThreadStatus::Success) {
                break;
            }
        }
        start.Signal();
        for (usize i = 0; i < started; ++i) {
            if (threads[i].WaitForCompletion(10000) != platform::WaitResult::Signaled ||
                threads[i].Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Name collision test worker did not finish");
            }
        }
        REQUIRE(started == 8);
        for (usize i = 0; i < 8; ++i) {
            CHECK(workers[i].readCorrect);
        }
        CHECK(workers[0].result == workers[1].result);
        CHECK(workers[2].result == workers[7].result);
        CHECK(workers[3].result == workers[4].result);
        CHECK(workers[0].result != workers[2].result);
        CHECK(workers[0].result != workers[3].result);
        CHECK(workers[0].result != workers[6].result);
        CHECK(workers[3].result != workers[5].result);
        CHECK(pool.Stats().entryCount == 8);
        CHECK(pool.Stats().activeLeases == 0);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
