#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/FrameArena.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>

using namespace rocks;
namespace frame_arena_tests_file {

using namespace rocks::platform;

namespace {

// backing 호출 수를 센다. 블록 재사용·반환 정책을 확인한다.
class CountingAllocator final : public Allocator {
public:
    explicit CountingAllocator(Allocator& base) noexcept : base_(base) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        auto result = base_.Allocate(bytes, alignment, tag);
        if (result) {
            AtomicFetchAdd(&allocations, uint64{1});
        }

        return result;
    }

    Result<void> Deallocate(void* address) noexcept override {
        auto result = base_.Deallocate(address);
        if (result && address != nullptr) {
            AtomicFetchAdd(&deallocations, uint64{1});
        }

        return result;
    }

    uint64 allocations = 0;
    uint64 deallocations = 0;

private:
    Allocator& base_;
};

// 소멸 순서를 기록한다.
struct OrderLog {
    uint32 values[64]{};
    uint32 count = 0;
};

struct Tracked {
    Tracked(OrderLog& log, uint32 value) noexcept : log_(&log), value_(value) {
    }

    ~Tracked() noexcept {
        log_->values[log_->count++] = value_;
    }

    OrderLog* log_;
    uint32 value_;
};

struct ArrayElement {
    static OrderLog* log;
    static uint32 next;
    uint32 value;

    ArrayElement() noexcept : value(next++) {
    }

    ~ArrayElement() noexcept {
        log->values[log->count++] = value;
    }
};

OrderLog* ArrayElement::log = nullptr;
uint32 ArrayElement::next = 0;

struct Plain {
    uint64 a = 1;
    uint64 b = 2;
};

struct alignas(256) Wide {
    uint8 bytes[256]{};
};

// 소멸 중 같은 arena를 다시 쓰려는 객체다.
struct Reentrant {
    FrameArena* arena;
    ErrorCode* allocate;
    ErrorCode* reset;

    ~Reentrant() noexcept {
        *allocate = arena->TryAllocate(16, 8).Error();
        *reset = arena->Reset().Error();
    }
};

struct Counted {
    static uint64 destroyed;
    uint64 owner;
    uint64 index;

    Counted(uint64 ownerId, uint64 position) noexcept : owner(ownerId), index(position) {
    }

    ~Counted() noexcept {
        AtomicFetchAdd(&destroyed, uint64{1});
    }
};

uint64 Counted::destroyed = 0;

struct WriterContext {
    FrameArena* arena;
    ManualResetEvent* launch;
    uint64 owner = 0;
    Counted* objects[2000]{};
    bool ok = true;
};

void WriteObjects(void* raw) noexcept {
    auto& context = *static_cast<WriterContext*>(raw);
    context.ok = context.launch->Wait(10000) == WaitResult::Signaled;
    FrameArenaWriter writer(*context.arena);
    for (uint64 index = 0; index < 2000 && context.ok; ++index) {
        auto object = writer.TryNew<Counted>(context.owner, index);
        if (!object) {
            context.ok = false;
            break;
        }

        context.objects[index] = object.Value();
        // 소멸자 없는 할당도 섞는다(다른 크기·정렬).
        auto scratch = writer.TryAllocate(24 + (index % 7) * 8, usize{8} << (index % 4));
        context.ok = scratch.IsOk() && scratch.Value() != nullptr;
    }
}

} // namespace

TEST(FrameArena_ResetRunsDestructorsInReverseRegistrationOrder) {
    SystemAllocator system;
    FrameArena arena(system);
    OrderLog log;
    ArrayElement::log = &log;
    ArrayElement::next = 10;

    REQUIRE(arena.TryNew<Tracked>(log, 1));
    auto plain = arena.TryNew<Plain>();
    REQUIRE(plain);
    CHECK(plain.Value()->a == 1 && plain.Value()->b == 2);
    auto array = arena.TryNewArray<ArrayElement>(3);
    REQUIRE(array);
    CHECK(array.Value().Size() == 3);
    CHECK(array.Value()[0].value == 10 && array.Value()[2].value == 12);
    REQUIRE(arena.TryNew<Tracked>(log, 2));
    auto empty = arena.TryNewArray<ArrayElement>(0);
    REQUIRE(empty);
    CHECK(empty.Value().Size() == 0);

    // 자명한 소멸자(Plain)는 등록하지 않는다.
    CHECK(arena.Stats().destructors == 3);
    CHECK(log.count == 0);
    REQUIRE(arena.Reset());
    // 등록 역순: Tracked 2 → 배열(원소 역순 12, 11, 10) → Tracked 1.
    const uint32 expected[] = {2, 12, 11, 10, 1};
    REQUIRE(log.count == 5);
    for (uint32 index = 0; index < 5; ++index) {
        CHECK(log.values[index] == expected[index]);
    }

    const FrameArenaStats stats = arena.Stats();
    CHECK(stats.destructors == 0);
    CHECK(stats.requestedBytes == 0);
    CHECK(stats.resets == 1);

    // 다음 수명은 새 목록이다. 두 번째 Reset은 이전 객체를 다시 부수지 않는다.
    REQUIRE(arena.Reset());
    CHECK(log.count == 5);
}

TEST(FrameArena_AlignmentLargeAllocationsAndErrors) {
    SystemAllocator system;
    CountingAllocator counting(system);
    FrameArenaConfig config;
    config.blockBytes = 8192;
    FrameArena arena(counting, config);

    for (usize alignment = 1; alignment <= 1024; alignment <<= 1) {
        auto memory = arena.TryAllocate(3, alignment);
        REQUIRE(memory);
        CHECK(reinterpret_cast<uptr>(memory.Value()) % alignment == 0);
    }

    auto wide = arena.TryNew<Wide>();
    REQUIRE(wide);
    CHECK(reinterpret_cast<uptr>(wide.Value()) % 256 == 0);
    CHECK(arena.Stats().largeBlocks == 0);

    // payload 절반을 넘는 요청과 블록보다 큰 요청은 각자 대형 블록이다.
    auto half = arena.TryAllocate(5000, 16);
    REQUIRE(half);
    auto huge = arena.TryAllocate(100000, 4096);
    REQUIRE(huge);
    CHECK(reinterpret_cast<uptr>(huge.Value()) % 4096 == 0);
    MemorySet(huge.Value(), 0xAB, 100000);
    CHECK(arena.Stats().largeBlocks == 2);

    // 0 byte는 성공한 nullptr, 잘못된 정렬은 0 byte여도 InvalidArgument, 크기 넘침은 Overflow다.
    auto zero = arena.TryAllocate(0, 8);
    REQUIRE(zero);
    CHECK(zero.Value() == nullptr);
    CHECK(arena.TryAllocate(0, 3).Error() == ErrorCode::InvalidArgument);
    CHECK(arena.TryAllocate(8, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(arena.TryAllocate(MaxSize, 16).Error() == ErrorCode::Overflow);
    CHECK(arena.TryNewArray<uint64>(MaxSize / 4).Error() == ErrorCode::Overflow);

    // 대형 블록은 Reset에서 바로 backing에 돌아간다.
    const uint64 before = counting.deallocations;
    REQUIRE(arena.Reset());
    CHECK(counting.deallocations == before + 2);
    CHECK(arena.Stats().largeBlocks == 0);
}

TEST(FrameArena_ResetRetainsBlocksAndTrimReturnsThem) {
    SystemAllocator system;
    CountingAllocator counting(system);
    FrameArenaConfig config;
    config.blockBytes = 4096;
    config.maxRetainedBlocks = 3;
    {
        FrameArena arena(counting, config);
        // 1 KiB 할당 12개면 표준 블록 여러 개가 필요하다.
        for (uint32 index = 0; index < 12; ++index) {
            REQUIRE(arena.TryAllocate(1024, 16));
        }

        const usize used = arena.Stats().usedBlocks;
        CHECK(used >= 4);
        CHECK(counting.allocations == used);

        // 세 개만 남기고 나머지는 돌려준다.
        REQUIRE(arena.Reset());
        CHECK(arena.Stats().retainedBlocks == 3);
        CHECK(counting.deallocations == used - 3);

        // 다음 수명이 남긴 블록 안에서 끝나면 backing을 부르지 않는다.
        const uint64 allocations = counting.allocations;
        for (uint32 index = 0; index < 9; ++index) {
            REQUIRE(arena.TryAllocate(1024, 16));
        }

        CHECK(counting.allocations == allocations);
        CHECK(arena.Stats().retainedBlocks == 0);
        REQUIRE(arena.Reset());

        // Trim은 남긴 블록을 모두 돌려준다.
        REQUIRE(arena.Trim());
        CHECK(arena.Stats().retainedBlocks == 0);
        CHECK(counting.deallocations == counting.allocations);

        // maxRetainedBlocks 0이면 하나도 남기지 않는다.
        FrameArenaConfig none = config;
        none.maxRetainedBlocks = 0;
        FrameArena strict(counting, none);
        REQUIRE(strict.TryAllocate(1024, 16));
        REQUIRE(strict.Reset());
        CHECK(strict.Stats().retainedBlocks == 0);
        CHECK(counting.deallocations == counting.allocations);

        REQUIRE(arena.TryAllocate(1024, 16));
    }

    // 파괴는 Reset 뒤 Trim이다.
    CHECK(counting.deallocations == counting.allocations);
}

TEST(FrameArena_WritersAllocateConcurrently) {
    SystemAllocator system;
    CountingAllocator counting(system);
    FrameArenaConfig config;
    config.blockBytes = 16384;
    FrameArena arena(counting, config);
    Counted::destroyed = 0;

    static WriterContext contexts[4];
    ManualResetEvent launch;
    Thread threads[4];
    for (uint32 index = 0; index < 4; ++index) {
        contexts[index] = WriterContext{&arena, &launch, index + 1};
        REQUIRE(threads[index].Start(WriteObjects, &contexts[index]) == ThreadStatus::Success);
    }

    launch.Signal();
    for (Thread& thread : threads) {
        if (thread.WaitForCompletion(10000) != WaitResult::Signaled ||
            thread.Join() != ThreadStatus::Success) {
            Fatal("FrameArena writer thread failed to terminate");
        }
    }

    // 각 writer가 만든 객체가 서로 덮어쓰지 않았는지 내용으로 확인한다.
    for (const WriterContext& context : contexts) {
        CHECK(context.ok);
        for (uint64 index = 0; index < 2000; ++index) {
            const Counted* object = context.objects[index];
            REQUIRE(object != nullptr);
            CHECK(object->owner == context.owner);
            CHECK(object->index == index);
            CHECK(reinterpret_cast<uptr>(object) % alignof(Counted) == 0);
        }
    }

    const FrameArenaStats stats = arena.Stats();
    CHECK(stats.destructors == 8000);
    CHECK(stats.openWriters == 0);
    CHECK(stats.usedBlocks >= 4);
    REQUIRE(arena.Reset());
    CHECK(Counted::destroyed == 8000);
}

TEST(FrameArena_ResetRejectsOpenWritersAndReentry) {
    SystemAllocator system;
    FrameArena arena(system);
    {
        FrameArenaWriter writer(arena);
        REQUIRE(writer.TryNew<Plain>());
        CHECK(arena.Stats().openWriters == 1);
        // 열린 writer가 있으면 Busy이고 아무것도 바꾸지 않는다.
        CHECK(arena.Reset().Error() == ErrorCode::Busy);
        CHECK(arena.Stats().usedBlocks == 1);
        CHECK(writer.TryAllocate(0, 6).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(arena.Stats().openWriters == 0);

    // 소멸자 안의 할당·Reset은 Busy다.
    ErrorCode allocate = ErrorCode::None;
    ErrorCode reset = ErrorCode::None;
    REQUIRE(arena.TryNew<Reentrant>(&arena, &allocate, &reset));
    REQUIRE(arena.Reset());
    CHECK(allocate == ErrorCode::Busy);
    CHECK(reset == ErrorCode::Busy);

    // backing 실패는 그 오류이고 이미 받은 할당은 그대로다.
    test::NFailAllocator failing(system);
    FrameArenaConfig config;
    config.blockBytes = 4096;
    FrameArena limited(failing, config);
    auto first = limited.TryNew<Plain>();
    REQUIRE(first);
    failing.FailOnAttempt(1);
    CHECK(limited.TryAllocate(4000, 8).Error() == ErrorCode::OutOfMemory);
    CHECK(first.Value()->a == 1);
    failing.DisableFailure();
    CHECK(limited.TryAllocate(4000, 8));
}
} // namespace frame_arena_tests_file
