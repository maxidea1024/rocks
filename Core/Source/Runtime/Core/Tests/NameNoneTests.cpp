#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

struct NoneWorker {
    NamePool* pool = nullptr;
    platform::ManualResetEvent* start = nullptr;
    StringView input;
    StringView expected;
    Name name{};
    bool passed = false;
};

void InternNone(void* raw) noexcept {
    auto& worker = *static_cast<NoneWorker*>(raw);
    if (worker.start->Wait(10000) != platform::WaitResult::Signaled) {
        return;
    }
    auto value = worker.pool->TryIntern(worker.input);
    if (!value) {
        return;
    }
    worker.name = value.Value();
    auto lease = worker.pool->Acquire(worker.name);
    worker.passed = lease && lease.Value().View() == worker.expected;
}

class BlockOnceAllocator final : public Allocator {
public:
    explicit BlockOnceAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        arrived.Signal();
        if (released.Wait(10000) != platform::WaitResult::Signaled) {
            platform::Fatal("Name None test allocation timed out");
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    Allocator& backing_;
    platform::ManualResetEvent arrived;
    platform::ManualResetEvent released;
};

struct InternShutdownWorker {
    NamePool* pool;
    ErrorCode result = ErrorCode::None;
};

void InternDuringShutdown(void* raw) noexcept {
    auto& worker = *static_cast<InternShutdownWorker*>(raw);
    auto result = worker.pool->TryIntern("nOnE");
    worker.result = result.Error();
}

} // namespace

TEST(NameNoneDefaultAndValidEmptyBaseMatchUe56) {
    const Name none;
    NameLease emptyLease;
    CHECK(!emptyLease && emptyLease.Identifier().IsNone());
    CHECK(emptyLease.View().IsEmpty());
    CHECK(none.IsValid() && none.IsNone());
    CHECK(!none.HasNumber() && none.GetNumber() == -1);

    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        CHECK(pool.TryIntern("").Error() == ErrorCode::ShuttingDown);
        REQUIRE(pool.Initialize());
        const StringView canonical[] = {StringView(nullptr), "", "None", "_0", "_7", "_2147483646"};
        const auto before = allocator.Stats().totalAllocations;
        for (StringView input : canonical) {
            auto name = pool.TryIntern(input);
            REQUIRE(name);
            CHECK(name.Value().IsValid() && name.Value().IsNone());
            CHECK(name.Value() == none);
            CHECK(Hash<Name>{}(name.Value()) == Hash<Name>{}(none));
            auto lease = pool.Acquire(name.Value());
            REQUIRE(lease);
            CHECK(lease.Value().View() == StringView("None"));
            CHECK(lease.Value().BaseView() == StringView("None"));
            CHECK(lease.Value().Identifier().IsNone());
        }
        CHECK(allocator.Stats().totalAllocations == before);
        CHECK(pool.Stats().entryCount == 0);
        auto large = pool.TryIntern("_2147483647");
        auto zeroes = pool.TryIntern("_00");
        auto underscore = pool.TryIntern("__7");
        REQUIRE(large && zeroes && underscore);
        CHECK(!large.Value().IsNone() && !large.Value().HasNumber());
        CHECK(!zeroes.Value().IsNone() && !zeroes.Value().HasNumber());
        CHECK(!underscore.Value().IsNone() && underscore.Value().GetNumber() == 7);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameNonePreservesDisplayAndNumberAcrossPools) {
    SystemAllocator allocator;
    {
        NamePool first(allocator);
        NamePool second(allocator);
        REQUIRE(first.Initialize() && second.Initialize());
        auto a = first.TryIntern("none");
        auto b = second.TryIntern("nOnE");
        auto firstNumber = first.TryIntern("None_7");
        auto secondNumber = second.TryIntern("none_7");
        auto zero = first.TryIntern("None_0");
        REQUIRE(a && b && firstNumber && secondNumber && zero);
        CHECK(a.Value().IsNone() && b.Value().IsNone());
        CHECK(a.Value() == b.Value() && a.Value() == Name{});
        CHECK(Hash<Name>{}(a.Value()) == Hash<Name>{}(Name{}));
        CHECK(firstNumber.Value() == secondNumber.Value());
        CHECK(Hash<Name>{}(firstNumber.Value()) == Hash<Name>{}(secondNumber.Value()));
        CHECK(!firstNumber.Value().IsNone() && firstNumber.Value().GetNumber() == 7);
        CHECK(firstNumber.Value() != Name{} && firstNumber.Value() != zero.Value());
        {
            HashSet<Name> names(allocator);
            REQUIRE(names.Add(Name{}));
            auto duplicate = names.Add(a.Value());
            REQUIRE(duplicate);
            CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
            CHECK(names.Contains(b.Value()));
            CHECK(!names.Contains(firstNumber.Value()));
            REQUIRE(names.Add(firstNumber.Value()));
            CHECK(names.Contains(secondNumber.Value()));
        }
        auto display = second.Acquire(b.Value());
        auto number = second.Acquire(secondNumber.Value());
        REQUIRE(display && number);
        CHECK(display.Value().View() == StringView("nOnE"));
        CHECK(number.Value().View() == StringView("none_7"));
        CHECK(number.Value().BaseView() == StringView("none"));
        CHECK(first.Acquire(b.Value()).Error() == ErrorCode::StaleHandle);
        CHECK(first.Acquire(secondNumber.Value()).Error() == ErrorCode::StaleHandle);
        display.Value().Reset();
        number.Value().Reset();
        const Name stale = a.Value();
        REQUIRE(first.Shutdown());
        REQUIRE(first.Initialize());
        CHECK(first.Acquire(stale).Error() == ErrorCode::StaleHandle);
        CHECK(stale.IsNone() && stale == Name{}); // 동일성 비교는 stale 검증을 대신하지 않는다.
        auto freshNone = first.Acquire(Name{});
        REQUIRE(freshNone);
        freshNone.Value().Reset();
        REQUIRE(first.Shutdown());
        REQUIRE(second.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameNoneLeaseAndMalformedTokensRespectLifetime) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        auto value = pool.TryIntern("nOnE");
        auto normal = pool.TryIntern("normal");
        REQUIRE(value && normal);
        Name broken = value.Value();
        broken.domain = 0;
        CHECK(!broken.IsValid() && !broken.IsNone());
        CHECK(broken != Name{});
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = value.Value();
        broken.kind = NameKind::Normal;
        CHECK(!broken.IsValid());
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = normal.Value();
        broken.id = 0;
        CHECK(!broken.IsValid());
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = Name{};
        broken.number = 1;
        CHECK(!broken.IsValid() && !broken.IsNone());
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        broken = value.Value();
        broken.displayId = normal.Value().displayId;
        CHECK(broken.IsValid()); // 형태만 유효하며 발급 여부는 Acquire에서 확인한다.
        CHECK(pool.Acquire(broken).Error() == ErrorCode::StaleHandle);
        constexpr char nul[] = {'N', 'o', 'n', 'e', '\0'};
        auto embedded = pool.TryIntern(StringView(nul, sizeof(nul)));
        REQUIRE(embedded);
        CHECK(!embedded.Value().IsNone());

        auto lease = pool.Acquire(Name{});
        REQUIRE(lease);
        auto copy = lease.Value().TryClone();
        REQUIRE(copy);
        CHECK(pool.Stats().activeLeases == 2);
        auto closed = pool.Shutdown();
        CHECK(!closed && closed.Error() == ErrorCode::Busy);
        CHECK(lease.Value().View() == StringView("None"));
        CHECK(lease.Value().TryClone().Error() == ErrorCode::ShuttingDown);
        CHECK(pool.Acquire(Name{}).Error() == ErrorCode::ShuttingDown);
        lease.Value().Reset();
        copy.Value().Reset();
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameNoneBudgetAndFailureLeavePriorState) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    {
        NamePool pool(allocator);
        NamePoolConfig config;
        config.maxEntries = 0;
        config.maxStorageBytes = 0;
        REQUIRE(pool.Initialize(config));
        allocator.FailOnAttempt(1);
        auto none = pool.TryIntern("None");
        auto empty = pool.TryIntern("");
        auto suffix = pool.TryIntern("_7");
        REQUIRE(none && empty && suffix);
        CHECK(none.Value().IsNone() && empty.Value().IsNone() && suffix.Value().IsNone());
        CHECK(allocator.Attempts() == 0 && pool.Stats().entryCount == 0);
        CHECK(pool.TryIntern("nOnE").Error() == ErrorCode::CapacityExceeded);
        CHECK(pool.TryIntern("None_7").Error() == ErrorCode::CapacityExceeded);
        REQUIRE(pool.Shutdown());

        allocator.DisableFailure();
        REQUIRE(pool.Initialize());
        allocator.FailOnAttempt(1);
        CHECK(pool.TryIntern("nOnE").Error() == ErrorCode::OutOfMemory);
        CHECK(pool.Stats().entryCount == 0 && pool.Stats().pendingEntries == 0 &&
              pool.Stats().activeOperations == 0);
        auto stillNone = pool.Acquire(Name{});
        REQUIRE(stillNone);
        stillNone.Value().Reset();
        allocator.DisableFailure();
        auto retry = pool.TryIntern("nOnE");
        REQUIRE(retry);
        CHECK(retry.Value().IsNone());
        allocator.FailOnAttempt(1);
        CHECK(pool.TryIntern("None_7").Error() == ErrorCode::OutOfMemory);
        CHECK(pool.Stats().entryCount == 1 && pool.Stats().pendingEntries == 0);
        allocator.DisableFailure();
        auto number = pool.TryIntern("None_7");
        REQUIRE(number);
        CHECK(!number.Value().IsNone() && number.Value().GetNumber() == 7);
        REQUIRE(pool.Shutdown());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(NameConcurrentNoneCasesRemainConsistent) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        platform::ManualResetEvent start;
        const StringView inputs[] = {"none", "NONE", "nOnE", "None", "_7", "None_7", "none_7", ""};
        const StringView displays[] = {"none", "NONE",   "nOnE",   "None",
                                       "None", "None_7", "none_7", "None"};
        NoneWorker workers[8]{};
        platform::Thread threads[8];
        usize started = 0;
        for (; started < 8; ++started) {
            workers[started] = NoneWorker{&pool, &start, inputs[started], displays[started]};
            if (threads[started].Start(InternNone, &workers[started]) !=
                platform::ThreadStatus::Success) {
                break;
            }
        }
        start.Signal();
        for (usize index = 0; index < started; ++index) {
            if (threads[index].WaitForCompletion(10000) != platform::WaitResult::Signaled ||
                threads[index].Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Name None test worker did not finish");
            }
        }
        REQUIRE(started == 8);
        for (usize index = 0; index < 8; ++index) {
            CHECK(workers[index].passed);
            if (index == 5 || index == 6) {
                CHECK(!workers[index].name.IsNone() && workers[index].name.GetNumber() == 7);
                CHECK(workers[index].name == workers[5].name);
            } else {
                CHECK(workers[index].name.IsNone() && workers[index].name == Name{});
            }
        }
        CHECK(pool.Stats().entryCount == 5);
        CHECK(pool.Stats().activeLeases == 0);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameNoneCaseCandidateCannotOutlivePoolShutdown) {
    SystemAllocator system;
    {
        BlockOnceAllocator blocking(system);
        NamePool pool(blocking);
        REQUIRE(pool.Initialize());
        InternShutdownWorker work{&pool};
        platform::Thread worker;
        REQUIRE(worker.Start(InternDuringShutdown, &work) == platform::ThreadStatus::Success);
        if (blocking.arrived.Wait(10000) != platform::WaitResult::Signaled) {
            blocking.released.Signal();
            CHECK(worker.Join() == platform::ThreadStatus::Success);
            CHECK(false);
            return;
        }
        CHECK(pool.Shutdown().Error() == ErrorCode::Busy);
        blocking.released.Signal();
        if (worker.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            worker.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Name None shutdown test worker did not finish");
        }
        CHECK(work.result == ErrorCode::ShuttingDown);
        CHECK(pool.Stats().pendingEntries == 0 && pool.Stats().activeOperations == 0);
        REQUIRE(pool.Shutdown());
    }
    CHECK(system.Stats().liveAllocations == 0);
}
