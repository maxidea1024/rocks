#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/StackAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <new>

using namespace rocks;

namespace {

struct ForeignStackCall {
    StackAllocator* stack;
    void* address;
    StackMark mark;
    ErrorCode allocate = ErrorCode::None;
    ErrorCode deallocate = ErrorCode::None;
    ErrorCode tryMark = ErrorCode::None;
    ErrorCode rewind = ErrorCode::None;
    ErrorCode reset = ErrorCode::None;
    ErrorCode trim = ErrorCode::None;
};

void RunForeignStackCall(void* raw) noexcept {
    auto& call = *static_cast<ForeignStackCall*>(raw);
    // 스레드 오류는 정렬·null·상태 오류보다 먼저 진단한다.
    call.allocate = call.stack->Allocate(0, 3).Error();
    call.deallocate = call.stack->Deallocate(call.address).Error();
    call.tryMark = call.stack->TryMark().Error();
    call.rewind = call.stack->Rewind(call.mark).Error();
    call.reset = call.stack->Reset().Error();
    call.trim = call.stack->Trim().Error();
}

class ReenteringStackBacking final : public Allocator {
public:
    explicit ReenteringStackBacking(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (stack != nullptr) {
            duringAllocate = stack->Allocate(8, 8).Error();
            duringAllocateMark = stack->TryMark().Error();
        }
        lastTag = tag;
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (stack != nullptr) {
            duringDeallocate = stack->Trim().Error();
            duringDeallocateMark = stack->TryMark().Error();
        }
        return backing_.Deallocate(address);
    }

    Allocator& backing_;
    StackAllocator* stack = nullptr;
    ErrorCode duringAllocate = ErrorCode::None;
    ErrorCode duringAllocateMark = ErrorCode::None;
    ErrorCode duringDeallocate = ErrorCode::None;
    ErrorCode duringDeallocateMark = ErrorCode::None;
    uint32 lastTag = 0;
};

} // namespace

TEST(CoreStackMarksTrackLifoAndInvalidateDiscardedBoundaries) {
    SystemAllocator backing;
    {
        StackAllocator stack(backing, 96);
        StackAllocator foreign(backing, 96);
        StackMark empty = stack.Mark();
        CHECK(stack.Rewind(StackMark{}).Error() == ErrorCode::InvalidArgument);
        CHECK(foreign.Rewind(empty).Error() == ErrorCode::InvalidArgument);

        auto first = stack.Allocate(17, 16);
        REQUIRE(first);
        CHECK(reinterpret_cast<uptr>(first.Value()) % 16 == 0);
        StackMark firstMark = stack.Mark();

        auto second = stack.Allocate(33, 64);
        REQUIRE(second);
        CHECK(reinterpret_cast<uptr>(second.Value()) % 64 == 0);
        StackMark secondMark = stack.Mark();
        CHECK(stack.Rewind(empty).Error() == ErrorCode::Busy);
        CHECK(stack.Rewind(firstMark).Error() == ErrorCode::Busy);
        CHECK(stack.Deallocate(first.Value()).Error() == ErrorCode::Busy);
        CHECK(stack.Reset().Error() == ErrorCode::Busy);
        CHECK(stack.Trim().Error() == ErrorCode::Busy);
        CHECK(stack.Stats().allocations.liveAllocations == 2);

        REQUIRE(stack.Deallocate(second.Value()));
        REQUIRE(stack.Rewind(firstMark));
        REQUIRE(stack.Rewind(firstMark));
        CHECK(stack.Rewind(secondMark).Error() == ErrorCode::StaleHandle);
        CHECK(stack.Deallocate(second.Value()).Error() == ErrorCode::NotFound);

        // 같은 주소를 재사용해도 반환된 표시의 식별자는 새 할당과 같아지지 않는다.
        auto replacement = stack.Allocate(33, 64);
        REQUIRE(replacement);
        CHECK(stack.Rewind(secondMark).Error() == ErrorCode::StaleHandle);
        REQUIRE(stack.Deallocate(replacement.Value()));
        REQUIRE(stack.Deallocate(first.Value()));
        CHECK(stack.Rewind(firstMark).Error() == ErrorCode::StaleHandle);
        REQUIRE(stack.Rewind(empty));

        const auto beforeReset = stack.Stats();
        REQUIRE(stack.Reset());
        CHECK(stack.Rewind(empty).Error() == ErrorCode::StaleHandle);
        CHECK(stack.Stats().reservedBytes == beforeReset.reservedBytes);
        CHECK(stack.Stats().chunks == beforeReset.chunks);
        StackMark afterReset = stack.Mark();
        REQUIRE(stack.Trim());
        CHECK(stack.Rewind(afterReset).Error() == ErrorCode::StaleHandle);
        CHECK(stack.Stats().reservedBytes == 0);
        CHECK(stack.Stats().chunks == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreStackRejectsForeignPointersAndBadAlignment) {
    SystemAllocator backing;
    {
        StackAllocator stack(backing, 256);
        CHECK(stack.Allocate(1, 0).Error() == ErrorCode::InvalidArgument);
        CHECK(stack.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        auto empty = stack.Allocate(0, 8);
        REQUIRE(empty);
        CHECK(empty.Value() == nullptr);
        REQUIRE(stack.Deallocate(nullptr));
        CHECK(stack.Allocate(MaxSize, 64).Error() == ErrorCode::Overflow);

        int32 local = 0;
        CHECK(stack.Deallocate(&local).Error() == ErrorCode::NotFound);
        CHECK(stack.Deallocate(reinterpret_cast<void*>(static_cast<uptr>(1))).Error() ==
              ErrorCode::NotFound);
        for (usize alignment = 1; alignment <= 4096; alignment *= 2) {
            auto allocation = stack.Allocate(19, alignment);
            REQUIRE(allocation);
            auto* memory = static_cast<uint8*>(allocation.Value());
            CHECK(reinterpret_cast<uptr>(memory) % alignment == 0);
            CHECK(stack.Deallocate(memory + 1).Error() == ErrorCode::NotFound);
            for (usize i = 0; i < 19; ++i) {
                memory[i] = static_cast<uint8>(i);
            }
            REQUIRE(stack.Deallocate(memory));
            CHECK(stack.Deallocate(memory).Error() == ErrorCode::NotFound);
        }
        REQUIRE(stack.Trim());
        CHECK(stack.Stats().allocations.liveAllocations == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreStackAllocationFailurePreservesLiveAllocationsAndMarks) {
    SystemAllocator backing;
    test::NFailAllocator failing(backing);
    {
        // 작은 chunk를 사용해 다음 할당이 반드시 backing을 거치게 한다.
        StackAllocator stack(failing, 1);
        auto first = stack.Allocate(29, 8);
        REQUIRE(first);
        auto* bytes = static_cast<uint8*>(first.Value());
        bytes[0] = 91;
        bytes[28] = 42;
        StackMark mark = stack.Mark();
        const StackStats before = stack.Stats();
        failing.FailOnAttempt(1);
        auto failed = stack.Allocate(900, 128);
        CHECK(!failed);
        CHECK(failed.Error() == ErrorCode::OutOfMemory);
        CHECK(stack.Stats().reservedBytes == before.reservedBytes);
        CHECK(stack.Stats().chunks == before.chunks);
        CHECK(stack.Stats().allocations.liveAllocations == before.allocations.liveAllocations);
        CHECK(stack.Stats().allocations.liveBytes == before.allocations.liveBytes);
        CHECK(stack.Stats().allocations.totalAllocations == before.allocations.totalAllocations);
        CHECK(bytes[0] == 91 && bytes[28] == 42);
        REQUIRE(stack.Rewind(mark));
        failing.DisableFailure();
        auto next = stack.Allocate(900, 128);
        REQUIRE(next);
        CHECK(stack.Rewind(mark).Error() == ErrorCode::Busy);
        REQUIRE(stack.Deallocate(next.Value()));
        REQUIRE(stack.Rewind(mark));
        REQUIRE(stack.Deallocate(first.Value()));
        REQUIRE(stack.Trim());
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreStackRejectsCallsFromOtherThreadsBeforeOtherErrors) {
    SystemAllocator backing;
    {
        StackAllocator stack(backing, 128);
        auto allocation = stack.Allocate(32, 8);
        REQUIRE(allocation);
        ForeignStackCall call{&stack, allocation.Value(), stack.Mark()};
        platform::Thread worker;
        if (worker.Start(RunForeignStackCall, &call) != platform::ThreadStatus::Success ||
            worker.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            worker.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Stack allocator foreign-thread test failed to join");
        }
        CHECK(call.allocate == ErrorCode::WrongThread);
        CHECK(call.deallocate == ErrorCode::WrongThread);
        CHECK(call.tryMark == ErrorCode::WrongThread);
        CHECK(call.rewind == ErrorCode::WrongThread);
        CHECK(call.reset == ErrorCode::WrongThread);
        CHECK(call.trim == ErrorCode::WrongThread);
        CHECK(stack.Stats().allocations.liveAllocations == 1);
        REQUIRE(stack.Deallocate(allocation.Value()));
        REQUIRE(stack.Trim());
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreStackBackingCallbacksCannotReenterAndGuardRestores) {
    SystemAllocator backing;
    ReenteringStackBacking adapter(backing);
    {
        StackAllocator stack(adapter, 1);
        adapter.stack = &stack;
        auto allocated = stack.Allocate(32, 8, 73);
        REQUIRE(allocated);
        CHECK(adapter.lastTag == 73);
        CHECK(adapter.duringAllocate == ErrorCode::Busy);
        CHECK(adapter.duringAllocateMark == ErrorCode::Busy);
        REQUIRE(stack.TryMark());
        REQUIRE(stack.Deallocate(allocated.Value()));
        REQUIRE(stack.Trim());
        CHECK(adapter.duringDeallocate == ErrorCode::Busy);
        CHECK(adapter.duringDeallocateMark == ErrorCode::Busy);
        REQUIRE(stack.TryMark());
        adapter.stack = nullptr;
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreStackSupportsSingleReservedArrayBuffer) {
    SystemAllocator backing;
    {
        StackAllocator stack(backing, 256);
        StackMark empty = stack.Mark();
        {
            // 다른 할당 없이 한 번만 예약한 버퍼는 역순 소멸 규약을 만족한다.
            Array<int32> values(stack);
            REQUIRE(values.TryReserve(8));
            for (int32 value = 0; value != 8; ++value) {
                REQUIRE(values.TryPushBack(value));
            }
            CHECK(values.Size() == 8);
            CHECK(values[7] == 7);
            CHECK(stack.Rewind(empty).Error() == ErrorCode::Busy);
        }
        REQUIRE(stack.Rewind(empty));
        REQUIRE(stack.Trim());
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreStackRecreatedAtSameAddressRejectsOldMarks) {
    SystemAllocator backing;
    alignas(StackAllocator) unsigned char storage[sizeof(StackAllocator)];
    auto* first = ::new (storage) StackAllocator(backing, 128);
    StackMark old = first->Mark();
    first->~StackAllocator();

    // 동일 주소에 새 allocator가 생겨도 소유자 ID가 달라 과거 표시가 승인되지 않는다.
    auto* second = ::new (storage) StackAllocator(backing, 128);
    CHECK(second->Rewind(old).Error() == ErrorCode::InvalidArgument);
    REQUIRE(second->Rewind(second->Mark()));
    second->~StackAllocator();
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreStackRepeatedChunksPreserveLifoAndPayloads) {
    SystemAllocator backing;
    {
        StackAllocator stack(backing, 128);
        StackMark empty = stack.Mark();
        void* addresses[64]{};
        usize sizes[64]{};

        // 여러 chunk가 번갈아 재사용되어도 가장 최근 할당부터만 반환할 수 있다.
        for (usize i = 0; i < 64; ++i) {
            sizes[i] = 1 + (i * 13 % 97);
            const usize alignment = static_cast<usize>(1) << (i % 7);
            auto item = stack.Allocate(sizes[i], alignment);
            REQUIRE(item);
            addresses[i] = item.Value();
            auto* bytes = static_cast<uint8*>(addresses[i]);
            bytes[0] = static_cast<uint8>(i);
            bytes[sizes[i] - 1] = static_cast<uint8>(i);
        }
        CHECK(stack.Stats().allocations.liveAllocations == 64);
        for (usize i = 64; i != 0; --i) {
            auto* bytes = static_cast<uint8*>(addresses[i - 1]);
            CHECK(bytes[0] == static_cast<uint8>(i - 1));
            CHECK(bytes[sizes[i - 1] - 1] == static_cast<uint8>(i - 1));
            REQUIRE(stack.Deallocate(bytes));
        }
        REQUIRE(stack.Rewind(empty));
        REQUIRE(stack.Reset());
        REQUIRE(stack.Trim());
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

namespace rocks::test {

namespace {

void MarkStackOnForeignThread(void* raw) noexcept {
    (void)static_cast<StackAllocator*>(raw)->Mark();
}

} // namespace

// 반드시 외부 watchdog이 감시하는 별도 프로세스에서 실행한다.
int RunStackAllocatorNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator backing;
    StackAllocator stack(backing, 128);

    if (operation == 0) {
        auto memory = stack.Allocate(16, 8);
        if (!memory) {
            return 0;
        }
        return 0; // allocator 소멸 시 미반환 블록을 진단해야 한다.
    }

    if (operation == 1) {
        platform::Thread worker;
        if (worker.Start(MarkStackOnForeignThread, &stack) != platform::ThreadStatus::Success ||
            worker.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            worker.Join() != platform::ThreadStatus::Success) {
            return 0;
        }
        return 0;
    }

    if (operation == 2) {
        // Array 성장은 새 버퍼를 만든 뒤 이전 버퍼를 반환하므로 stack의 LIFO 계약과 다르다.
        Array<int32> values(stack);
        if (!values.TryReserve(1)) {
            return 0;
        }
        auto growth = values.TryReserve(2);
        (void)growth;
        return 0;
    }

    return 0;
}

} // namespace rocks::test
