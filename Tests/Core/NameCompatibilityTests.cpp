#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Platform/Thread.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

struct InternWorker {
    NamePool* pool;
    platform::ManualResetEvent* start;
    StringView input;
    Name name{};
    bool passed = false;
};

void InternOnWorker(void* raw) noexcept {
    auto& worker = *static_cast<InternWorker*>(raw);
    if (worker.start->Wait(10000) != platform::WaitResult::Signaled) {
        return;
    }
    auto result = worker.pool->TryIntern(worker.input);
    if (!result) {
        return;
    }
    worker.name = result.Value();
    auto lease = worker.pool->Acquire(worker.name);
    worker.passed = lease && lease.Value().View() == worker.input &&
                    lease.Value().BaseView().ByteCount() == worker.input.ByteCount() - 2;
}

} // namespace

TEST(NameCasePreservesEachSpellingAndHashIdentity) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        NamePoolConfig config;
        config.hashMask = 0; // 비교 이름 hash 충돌을 강제한다.
        REQUIRE(pool.Initialize(config));
        auto first = pool.TryIntern("Actor_7");
        auto second = pool.TryIntern("aCTOr_7");
        auto numbered = pool.TryIntern("ACTOR_8");
        auto plain = pool.TryIntern("actor");
        REQUIRE(first && second && numbered && plain);
        CHECK(first.Value() == second.Value());
        CHECK(first.Value().displayId != second.Value().displayId);
        CHECK(first.Value() != numbered.Value());
        CHECK(first.Value() != plain.Value());
        CHECK(first.Value().HasNumber() && first.Value().GetNumber() == 7);
        CHECK(!plain.Value().HasNumber() && plain.Value().GetNumber() == -1);
        CHECK(Hash<Name>{}(first.Value()) == Hash<Name>{}(second.Value()));

        {
            HashSet<Name> names(allocator);
            REQUIRE(names.Add(first.Value()));
            CHECK(names.Contains(second.Value()));
            CHECK(!names.Contains(numbered.Value()));
        }
        {
            auto firstLease = pool.Acquire(first.Value());
            auto secondLease = pool.Acquire(second.Value());
            REQUIRE(firstLease && secondLease);
            CHECK(firstLease.Value().View() == StringView("Actor_7"));
            CHECK(firstLease.Value().BaseView() == StringView("Actor"));
            CHECK(secondLease.Value().View() == StringView("aCTOr_7"));
            CHECK(secondLease.Value().BaseView() == StringView("aCTOr"));
            Name forged = first.Value();
            forged.displayId = numbered.Value().displayId;
            CHECK(pool.Acquire(forged).Error() == ErrorCode::StaleHandle);
        }
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameNumberSuffixMatchesDocumentedUe56Bounds) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        struct Case {
            const char* input;
            int64 number;
            const char* base;
        };
        const Case cases[] = {
            {"Rocket", -1, "Rocket"},
            {"Rocket_0", 0, "Rocket"},
            {"Rocket_19", 19, "Rocket"},
            {"Rocket_2147483646", 2147483646, "Rocket"},
            {"Rocket_2147483647", -1, "Rocket_2147483647"},
            {"Rocket_04", -1, "Rocket_04"},
            {"Rocket_00000000000", -1, "Rocket_00000000000"},
            {"Rocket_-1", -1, "Rocket_-1"},
            {"Rocket_", -1, "Rocket_"},
            {"Rocket__7", 7, "Rocket_"},
            {"_0", -1, "None"}, // 빈 base의 유효 번호는 UE5.6처럼 기본 None이다.
        };
        for (const Case& sample : cases) {
            auto name = pool.TryIntern(sample.input);
            REQUIRE(name);
            CHECK(name.Value().GetNumber() == sample.number);
            CHECK(name.Value().HasNumber() == (sample.number >= 0));
            auto lease = pool.Acquire(name.Value());
            REQUIRE(lease);
            CHECK(lease.Value().View() == (StringView(sample.input) == StringView("_0")
                                               ? StringView("None")
                                               : StringView(sample.input)));
            CHECK(lease.Value().BaseView() == StringView(sample.base));
        }
        auto number = pool.TryIntern("Rocket_4");
        auto leadingZero = pool.TryIntern("Rocket_04");
        REQUIRE(number && leadingZero);
        CHECK(number.Value() != leadingZero.Value());
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameAsciiFoldLeavesUnicodeBytesAndNulExplicit) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        constexpr char upper[] = {'A', '\0', 'Z'};
        constexpr char lower[] = {'a', '\0', 'z'};
        auto first = pool.TryIntern(StringView(upper, sizeof(upper)));
        auto second = pool.TryIntern(StringView(lower, sizeof(lower)));
        auto accentedUpper = pool.TryIntern("\xC3\x84"
                                            "A");
        auto accentedLower = pool.TryIntern("\xC3\xA4"
                                            "a");
        REQUIRE(first && second && accentedUpper && accentedLower);
        CHECK(first.Value() == second.Value());
        CHECK(accentedUpper.Value() != accentedLower.Value());
        auto lease = pool.Acquire(second.Value());
        REQUIRE(lease);
        CHECK(lease.Value().View() == StringView(lower, sizeof(lower)));
        lease.Value().Reset();
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameCaseVariantOomAndCapacityPreservePriorEntry) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        auto first = pool.TryIntern("Item_3");
        REQUIRE(first);
        allocator.FailOnAttempt(1);
        auto failed = pool.TryIntern("ITEM_3");
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(pool.Stats().entryCount == 1);
        CHECK(pool.Stats().pendingEntries == 0);
        CHECK(pool.Stats().activeOperations == 0);
        auto stillPresent = pool.Acquire(first.Value());
        REQUIRE(stillPresent);
        CHECK(stillPresent.Value().View() == StringView("Item_3"));
        stillPresent.Value().Reset();
        allocator.DisableFailure();
        auto retry = pool.TryIntern("ITEM_3");
        REQUIRE(retry);
        CHECK(retry.Value() == first.Value());
        REQUIRE(pool.Shutdown());
    }
    CHECK(system.Stats().liveAllocations == 0);
    {
        NamePool pool(system);
        NamePoolConfig config;
        config.maxEntries = 1;
        REQUIRE(pool.Initialize(config));
        auto initial = pool.TryIntern("Map");
        REQUIRE(initial);
        CHECK(pool.TryIntern("MAP").Error() == ErrorCode::CapacityExceeded);
        auto again = pool.TryIntern("Map");
        REQUIRE(again);
        CHECK(again.Value() == initial.Value());
        REQUIRE(pool.Shutdown());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(NameConcurrentCaseVariantsShareComparisonAndKeepDisplay) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        platform::ManualResetEvent start;
        const StringView spellings[] = {"Alpha_7", "ALPHA_7", "aLpHa_7", "alpha_7",
                                        "alpha_7", "Alpha_7", "ALPHA_7", "aLpHa_7"};
        InternWorker workers[8]{};
        platform::Thread threads[8];
        usize started = 0;
        for (; started < 8; ++started) {
            workers[started].pool = &pool;
            workers[started].start = &start;
            workers[started].input = spellings[started];
            if (threads[started].Start(InternOnWorker, &workers[started]) !=
                platform::ThreadStatus::Success) {
                break;
            }
        }
        start.Signal();
        for (usize index = 0; index < started; ++index) {
            if (threads[index].WaitForCompletion(10000) != platform::WaitResult::Signaled ||
                threads[index].Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Name compatibility test worker did not finish");
            }
        }
        REQUIRE(started == 8);
        for (usize index = 0; index < 8; ++index) {
            CHECK(workers[index].passed);
            CHECK(workers[index].name == workers[0].name);
            CHECK(workers[index].name.GetNumber() == 7);
        }
        CHECK(pool.Stats().entryCount == 4);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
