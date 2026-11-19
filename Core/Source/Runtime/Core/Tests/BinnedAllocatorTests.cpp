#include <Core/Containers/Array.h>
#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Private/Memory/BinnedAllocatorTestAccess.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// BinnedAllocator의 계약·소유 판정·slab 수명·실패 경로다(Docs/Design/GarbageCollectionScale.ko.md
// 18.4·18.5·18.9절).
using namespace rocks;
namespace binned_allocator_tests_file {

using rocks::detail::BinnedAllocatorTestAccess;

namespace {

constexpr usize SlabBytes = BinnedAllocator::SlabBytes;
constexpr usize MiB = usize{1} << 20;
// slab 앞 머리 크기다(첫 블록의 slab 안 offset).
constexpr usize SlabHeaderBytes = 64;
constexpr uint32 WorkerTimeoutMs = 60000;

// 상위 allocator의 반환을 거부하게 만들 수 있다.
class FreeFailureAllocator final : public Allocator {
public:
    explicit FreeFailureAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    bool failFree = false;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (failFree) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

// 요청한 정렬만 딱 지키는 상위 allocator다(그보다 큰 정렬은 일부러 어긴다 — 주소가 정렬의 홀수
// 배다).
class MinimalAlignmentBacking final : public Allocator {
public:
    explicit MinimalAlignmentBacking(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (count_ == Capacity) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        auto base = backing_.Allocate(bytes + alignment, alignment * 2, tag);
        if (!base) {
            return base;
        }

        void* shifted = static_cast<uint8*>(base.Value()) + alignment;
        entries_[count_++] = Entry{shifted, base.Value()};
        return Result<void*>::Success(shifted);
    }

    Result<void> Deallocate(void* address) noexcept override {
        for (uint32 index = 0; index < count_; ++index) {
            if (entries_[index].shifted == address) {
                void* base = entries_[index].base;
                entries_[index] = entries_[--count_];
                return backing_.Deallocate(base);
            }
        }

        return Result<void>::Failure(ErrorCode::NotFound);
    }

private:
    struct Entry {
        void* shifted = nullptr;
        void* base = nullptr;
    };

    static constexpr uint32 Capacity = 16;

    Allocator& backing_;
    Entry entries_[Capacity]{};
    uint32 count_ = 0;
};

// 큰 경로의 상위 호출 안에서 같은 allocator를 다시 부른다(상위 호출이 잠금 밖인지).
class ReentrantBacking final : public Allocator {
public:
    explicit ReentrantBacking(Allocator& backing) noexcept : backing_(backing) {
    }

    BinnedAllocator* target = nullptr;
    uint32 nestedSuccesses = 0;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        Nest();
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        Nest();
        return backing_.Deallocate(address);
    }

private:
    void Nest() noexcept {
        if (target == nullptr) {
            return;
        }

        BinnedAllocator* current = target;
        target = nullptr;
        // 작은 블록과 큰 블록을 하나씩 다시 받는다.
        auto small = current->Allocate(40, 16);
        auto large = current->Allocate(BinnedAllocator::MaxSmallBytes + 1, 16);
        if (small && large && current->Deallocate(small.Value()) &&
            current->Deallocate(large.Value())) {
            ++nestedSuccesses;
        }

        target = current;
    }

    Allocator& backing_;
};

// 결정적 의사 난수다(seed 고정).
struct BinnedRng {
    uint64 state;

    uint64 Next() noexcept {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 17;
    }
};

bool InsideRange(BinnedAllocator& allocator, const void* address) noexcept {
    return BinnedAllocatorTestAccess::SlabTableEntry(allocator, address) !=
           BinnedAllocatorTestAccess::OutsideRange;
}

usize BlockBytesOf(usize bytes, usize alignment) noexcept {
    return BinnedAllocatorTestAccess::ClassBlockBytes(
        BinnedAllocatorTestAccess::ClassOf(bytes, alignment));
}

void FillPattern(void* address, usize bytes, uint8 seed) noexcept {
    auto* data = static_cast<uint8*>(address);
    for (usize index = 0; index < bytes; ++index) {
        data[index] = static_cast<uint8>(seed + index * 7);
    }
}

bool CheckPattern(const void* address, usize bytes, uint8 seed) noexcept {
    const auto* data = static_cast<const uint8*>(address);
    for (usize index = 0; index < bytes; ++index) {
        if (data[index] != static_cast<uint8>(seed + index * 7)) {
            return false;
        }
    }

    return true;
}

bool SameStats(const AllocationStats& left, const AllocationStats& right) noexcept {
    return left.liveAllocations == right.liveAllocations && left.liveBytes == right.liveBytes &&
           left.totalAllocations == right.totalAllocations;
}

void StartWorker(platform::Thread& thread, platform::Thread::Entry entry, void* context) noexcept {
    if (thread.Start(entry, context) != platform::ThreadStatus::Success) {
        platform::Fatal("Binned test worker failed to start");
    }
}

void JoinWorker(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(WorkerTimeoutMs) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Binned test worker failed to finish");
    }
}

// 시작 문이다: 모든 worker가 문 앞에 선 뒤에 함께 연다(순서를 시간에 기대지 않는다).
struct StartGate {
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    uint32 waiting = 0;
    bool open = false;

    void Arrive() noexcept {
        platform::LockGuard lock(mutex);
        ++waiting;
        changed.NotifyAll();
        while (!open) {
            (void)changed.Wait(mutex);
        }
    }

    void OpenWhenArrived(uint32 workers) noexcept {
        platform::LockGuard lock(mutex);
        while (waiting != workers) {
            (void)changed.Wait(mutex);
        }

        open = true;
        changed.NotifyAll();
    }
};

struct StressWork {
    BinnedAllocator* allocator;
    StartGate* gate;
    uint32 number;
    uint32 failures = 0;
};

constexpr uint32 StressIterations = 2000;

void RunStressWork(void* context) noexcept {
    auto& work = *static_cast<StressWork*>(context);
    work.gate->Arrive();
    constexpr usize Window = 16;
    void* held[Window]{};
    usize heldBytes[Window]{};
    for (uint32 iteration = 0; iteration < StressIterations; ++iteration) {
        const usize slot = iteration % Window;
        if (held[slot] != nullptr) {
            if (!CheckPattern(held[slot], heldBytes[slot], static_cast<uint8>(work.number)) ||
                !work.allocator->Deallocate(held[slot])) {
                ++work.failures;
            }
        }

        const usize size = (iteration * 29 + work.number * 17) % 9000 + 1;
        auto result = work.allocator->Allocate(size, iteration % 7 == 0 ? 64 : 16, work.number);
        if (!result || result.Value() == nullptr) {
            ++work.failures;
            held[slot] = nullptr;
            continue;
        }

        held[slot] = result.Value();
        heldBytes[slot] = size < 64 ? size : 64;
        FillPattern(held[slot], heldBytes[slot], static_cast<uint8>(work.number));
    }

    for (usize slot = 0; slot < Window; ++slot) {
        if (held[slot] != nullptr &&
            (!CheckPattern(held[slot], heldBytes[slot], static_cast<uint8>(work.number)) ||
             !work.allocator->Deallocate(held[slot]))) {
            ++work.failures;
        }
    }
}

struct ReturnWork {
    BinnedAllocator* allocator;
    void** addresses;
    usize count;
    uint32 failures = 0;
};

void RunReturnWork(void* context) noexcept {
    auto& work = *static_cast<ReturnWork*>(context);
    for (usize index = 0; index < work.count; ++index) {
        if (!work.allocator->Deallocate(work.addresses[index])) {
            ++work.failures;
        }
    }
}

struct DuplicateWork {
    BinnedAllocator* allocator;
    StartGate* gate;
    void* address;
    ErrorCode result = ErrorCode::None;
};

void RunDuplicateWork(void* context) noexcept {
    auto& work = *static_cast<DuplicateWork*>(context);
    work.gate->Arrive();
    work.result = work.allocator->Deallocate(work.address).Error();
}

} // namespace

// 등급 표가 설계(18.5절)와 같다: 16..128은 16 단계, 그 위는 2배 구간마다 넷, 모두 16의 배수, 요청은
// 맞는 가장 작은 등급으로 가고 128을 넘는 요청의 올림 낭비는 블록의 20% 이하다.
TEST(CoreBinnedClassTableMatchesDesign) {
    constexpr usize Expected[] = {16,   32,   48,   64,   80,   96,   112,  128,  160,  192,  224,
                                  256,  320,  384,  448,  512,  640,  768,  896,  1024, 1280, 1536,
                                  1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192};
    constexpr uint32 ExpectedCount = sizeof(Expected) / sizeof(Expected[0]);
    REQUIRE(BinnedAllocatorTestAccess::ClassCount() == ExpectedCount);
    for (uint32 sizeClass = 0; sizeClass < ExpectedCount; ++sizeClass) {
        CHECK(BinnedAllocatorTestAccess::ClassBlockBytes(sizeClass) == Expected[sizeClass]);
        CHECK(BinnedAllocatorTestAccess::ClassCapacity(sizeClass) ==
              (SlabBytes - SlabHeaderBytes) / Expected[sizeClass]);
    }

    constexpr usize LinearLimit = 128;
    constexpr usize WasteDivisor = 5;
    for (usize bytes = 1; bytes <= BinnedAllocator::MaxSmallBytes; ++bytes) {
        const uint32 sizeClass = BinnedAllocatorTestAccess::ClassOf(bytes, 16);
        REQUIRE(sizeClass < ExpectedCount);
        const usize block = Expected[sizeClass];
        REQUIRE(block >= bytes);
        if (sizeClass != 0) {
            REQUIRE(Expected[sizeClass - 1] < bytes);
        }

        if (bytes > LinearLimit) {
            REQUIRE((block - bytes) * WasteDivisor <= block);
        }

        // 16 이하 정렬은 같은 등급이다.
        REQUIRE(BinnedAllocatorTestAccess::ClassOf(bytes, 1) == sizeClass);
        // 정렬 32·64는 블록 크기가 그 배수인 첫 등급이다.
        for (usize alignment = 32; alignment <= BinnedAllocator::MaxSmallAlignment;
             alignment *= 2) {
            const uint32 promoted = BinnedAllocatorTestAccess::ClassOf(bytes, alignment);
            REQUIRE(promoted >= sizeClass);
            REQUIRE(promoted < ExpectedCount);
            REQUIRE(Expected[promoted] % alignment == 0);
            for (uint32 skipped = sizeClass; skipped < promoted; ++skipped) {
                REQUIRE(Expected[skipped] % alignment != 0);
            }
        }
    }
}

// 반환의 블록 경계 판정(역수 상수 나눗셈)이 모든 등급·모든 offset에서 실제 나눗셈과 같다.
TEST(CoreBinnedBlockBoundaryDivisionIsExact) {
    for (uint32 sizeClass = 0; sizeClass < BinnedAllocatorTestAccess::ClassCount(); ++sizeClass) {
        const usize block = BinnedAllocatorTestAccess::ClassBlockBytes(sizeClass);
        for (usize offset = 0; offset < SlabBytes; offset += 8) {
            uint32 index = NumericLimits<uint32>::Max();
            const bool boundary = BinnedAllocatorTestAccess::BlockIndexOf(sizeClass, offset, index);
            const bool expected =
                offset >= SlabHeaderBytes && (offset - SlabHeaderBytes) % block == 0;
            REQUIRE(boundary == expected);
            if (expected) {
                REQUIRE(index == (offset - SlabHeaderBytes) / block);
            }
        }

        // 16의 배수가 아닌 offset은 경계가 아니다.
        uint32 index = 0;
        CHECK(!BinnedAllocatorTestAccess::BlockIndexOf(sizeClass, SlabHeaderBytes + 1, index));
        CHECK(!BinnedAllocatorTestAccess::BlockIndexOf(sizeClass, SlabHeaderBytes + block + 15,
                                                       index));
    }
}

// 0 byte·잘못된 정렬·등급 경계 크기·정렬 올림·큰 경로와 통계다.
TEST(CoreBinnedSizesAlignmentAndStats) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        CHECK(allocator.ReservedBytes() == BinnedAllocator::DefaultReserveBytes);
        auto empty = allocator.Allocate(0, 8);
        REQUIRE(empty);
        CHECK(empty.Value() == nullptr);
        CHECK(allocator.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(allocator.Allocate(16, 0).Error() == ErrorCode::InvalidArgument);
        CHECK(allocator.Allocate(16, 24).Error() == ErrorCode::InvalidArgument);
        // 상위 allocator의 오류는 그대로 전달한다.
        CHECK(allocator.Allocate(MaxSize, 16).Error() == ErrorCode::Overflow);
        REQUIRE(allocator.Deallocate(nullptr));
        CHECK(allocator.Stats().totalAllocations == 0);

        constexpr usize Sizes[] = {1,   15,  16,  17,  31,   32,   33,   127,  128,  129,  160,
                                   161, 255, 256, 257, 1024, 1025, 4096, 4097, 5000, 8191, 8192};
        constexpr usize SizeCount = sizeof(Sizes) / sizeof(Sizes[0]);
        void* blocks[SizeCount]{};
        uint64 expectedBytes = 0;
        for (usize index = 0; index < SizeCount; ++index) {
            auto result = allocator.Allocate(Sizes[index], 8, 42);
            REQUIRE(result);
            blocks[index] = result.Value();
            CHECK(reinterpret_cast<uptr>(blocks[index]) % 16 == 0);
            CHECK(InsideRange(allocator, blocks[index]));
            CHECK(BinnedAllocatorTestAccess::SlabTableEntry(allocator, blocks[index]) ==
                  BinnedAllocatorTestAccess::ClassOf(Sizes[index], 8) + 1);
            FillPattern(blocks[index], Sizes[index], static_cast<uint8>(index));
            expectedBytes += BlockBytesOf(Sizes[index], 8);
        }

        CHECK(allocator.Stats().liveAllocations == SizeCount);
        // liveBytes는 요청 크기가 아니라 블록 크기의 합이다.
        CHECK(allocator.Stats().liveBytes == expectedBytes);
        CHECK(allocator.Usage().largeAllocations == 0);
        CHECK(allocator.Usage().fallbackAllocations == 0);

        // 8192를 넘는 크기와 64를 넘는 정렬은 큰 경로다(예약 범위 밖, 요청 크기 그대로 센다).
        auto large = allocator.Allocate(BinnedAllocator::MaxSmallBytes + 1, 16);
        REQUIRE(large);
        CHECK(!InsideRange(allocator, large.Value()));
        auto aligned = allocator.Allocate(7, 4096);
        REQUIRE(aligned);
        CHECK(reinterpret_cast<uptr>(aligned.Value()) % 4096 == 0);
        CHECK(!InsideRange(allocator, aligned.Value()));
        CHECK(allocator.Usage().largeAllocations == 2);
        CHECK(allocator.Usage().fallbackAllocations == 0);
        CHECK(allocator.Stats().liveBytes ==
              expectedBytes + BinnedAllocator::MaxSmallBytes + 1 + 7);
        CHECK(system.Stats().liveAllocations == 2);

        // 정렬 32·64는 블록 주소가 정렬되고(여러 블록 모두) 예약 범위 안이다.
        constexpr usize AlignedSizes[] = {1, 17, 48, 80, 100, 200, 300, 5000};
        constexpr usize PerSize = 6;
        void* alignedBlocks[sizeof(AlignedSizes) / sizeof(AlignedSizes[0]) * 2 * PerSize]{};
        usize alignedCount = 0;
        for (usize alignment = 32; alignment <= BinnedAllocator::MaxSmallAlignment;
             alignment *= 2) {
            for (usize size : AlignedSizes) {
                for (usize repeat = 0; repeat < PerSize; ++repeat) {
                    auto result = allocator.Allocate(size, alignment);
                    REQUIRE(result);
                    CHECK(reinterpret_cast<uptr>(result.Value()) % alignment == 0);
                    CHECK(InsideRange(allocator, result.Value()));
                    alignedBlocks[alignedCount++] = result.Value();
                }
            }
        }

        for (usize index = 0; index < alignedCount; ++index) {
            REQUIRE(allocator.Deallocate(alignedBlocks[index]));
        }

        for (usize index = 0; index < SizeCount; ++index) {
            CHECK(CheckPattern(blocks[index], Sizes[index], static_cast<uint8>(index)));
            REQUIRE(allocator.Deallocate(blocks[index]));
        }

        REQUIRE(allocator.Deallocate(large.Value()));
        REQUIRE(allocator.Deallocate(aligned.Value()));
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
        CHECK(allocator.Stats().totalAllocations == SizeCount + 2 + alignedCount);
        CHECK(allocator.Usage().largeAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 미소유·중간·중복 주소의 반환은 NotFound이고 상태가 그대로다.
TEST(CoreBinnedOwnershipChecksRejectWithoutStateChange) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        BinnedAllocator other(system);
        auto small = allocator.Allocate(48, 16);
        auto tiny = allocator.Allocate(16, 16);
        auto large = allocator.Allocate(20000, 16);
        auto foreignSmall = other.Allocate(48, 16);
        auto foreignLarge = other.Allocate(20000, 16);
        REQUIRE(small);
        REQUIRE(tiny);
        REQUIRE(large);
        REQUIRE(foreignSmall);
        REQUIRE(foreignLarge);
        FillPattern(small.Value(), 48, 3);
        FillPattern(tiny.Value(), 16, 5);
        const AllocationStats before = allocator.Stats();
        const AllocationStats otherBefore = other.Stats();

        // 1단계: 예약 범위 밖이고 큰 할당 원장에도 없다(스택, 다른 인스턴스의 블록).
        int local = 0;
        CHECK(allocator.Deallocate(&local).Error() == ErrorCode::NotFound);
        CHECK(allocator.Deallocate(foreignSmall.Value()).Error() == ErrorCode::NotFound);
        CHECK(allocator.Deallocate(foreignLarge.Value()).Error() == ErrorCode::NotFound);
        CHECK(other.Deallocate(small.Value()).Error() == ErrorCode::NotFound);
        CHECK(other.Deallocate(large.Value()).Error() == ErrorCode::NotFound);
        // 큰 블록의 가운데 주소도 원장에 없다.
        CHECK(allocator.Deallocate(static_cast<uint8*>(large.Value()) + 16).Error() ==
              ErrorCode::NotFound);

        // 2단계: 예약 범위 안의 쓰지 않는 slab이다(commit되지 않은 주소 — 읽지 않는다).
        constexpr usize FarSlabs = 1000;
        auto* unused = static_cast<uint8*>(small.Value()) + FarSlabs * SlabBytes;
        REQUIRE(BinnedAllocatorTestAccess::SlabTableEntry(allocator, unused) == 0);
        CHECK(allocator.Deallocate(unused).Error() == ErrorCode::NotFound);

        // 3단계: 블록 경계가 아닌 주소(가운데, slab 머리)다.
        CHECK(allocator.Deallocate(static_cast<uint8*>(small.Value()) + 1).Error() ==
              ErrorCode::NotFound);
        CHECK(allocator.Deallocate(static_cast<uint8*>(small.Value()) + 16).Error() ==
              ErrorCode::NotFound);
        CHECK(allocator.Deallocate(static_cast<uint8*>(tiny.Value()) + 8).Error() ==
              ErrorCode::NotFound);
        const uptr slabStart = reinterpret_cast<uptr>(small.Value()) & ~(SlabBytes - 1);
        CHECK(allocator.Deallocate(reinterpret_cast<void*>(slabStart)).Error() ==
              ErrorCode::NotFound);
        CHECK(allocator.Deallocate(reinterpret_cast<void*>(slabStart + 16)).Error() ==
              ErrorCode::NotFound);

        CHECK(SameStats(allocator.Stats(), before));
        CHECK(SameStats(other.Stats(), otherBefore));
        CHECK(CheckPattern(small.Value(), 48, 3));
        CHECK(CheckPattern(tiny.Value(), 16, 5));

        // 5단계: 이미 반환한 블록이다(같은 slab에 살아 있는 블록이 남아 있어 slab은 그대로다).
        auto second = allocator.Allocate(48, 16);
        REQUIRE(second);
        REQUIRE(allocator.Deallocate(small.Value()));
        const AllocationStats afterFree = allocator.Stats();
        CHECK(allocator.Deallocate(small.Value()).Error() == ErrorCode::NotFound);
        CHECK(allocator.Deallocate(small.Value()).Error() == ErrorCode::NotFound);
        CHECK(SameStats(allocator.Stats(), afterFree));
        // 중복 반환이 자유 목록을 망가뜨리지 않았다: 이어지는 할당이 서로 다른 주소를 받는다.
        auto third = allocator.Allocate(48, 16);
        auto fourth = allocator.Allocate(48, 16);
        REQUIRE(third);
        REQUIRE(fourth);
        CHECK(third.Value() != fourth.Value());
        CHECK(third.Value() != second.Value());
        CHECK(fourth.Value() != second.Value());

        REQUIRE(allocator.Deallocate(large.Value()));
        CHECK(allocator.Deallocate(large.Value()).Error() == ErrorCode::NotFound);
        REQUIRE(allocator.Deallocate(second.Value()));
        REQUIRE(allocator.Deallocate(third.Value()));
        REQUIRE(allocator.Deallocate(fourth.Value()));
        REQUIRE(allocator.Deallocate(tiny.Value()));
        REQUIRE(other.Deallocate(foreignSmall.Value()));
        REQUIRE(other.Deallocate(foreignLarge.Value()));
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(other.Stats().liveAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 표지 불변식(18.4절, 검토 F1): 표지가 없는 블록은 사용자에게 나가 있는 블록뿐이다. 내준 적 없는
// 블록(바로 다음 블록, 아직 뗀 적 없는 블록)의 반환은 NotFound이고, 그 뒤의 할당들이 서로 다른
// 주소를 받는다. 보관소에서 다시 꺼낸 slab(옛 표지가 남은 slab)에서도 같다.
TEST(CoreBinnedNeverIssuedBlocksAreNotOwned) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        constexpr usize Request = 96;
        const usize block = BlockBytesOf(Request, 16);
        constexpr uint32 Later = 24;
        for (uint32 round = 0; round < 3; ++round) {
            auto first = allocator.Allocate(Request, 16);
            REQUIRE(first);
            auto* next = static_cast<uint8*>(first.Value()) + block;
            const AllocationStats before = allocator.Stats();
            // 같은 slab의 바로 다음 블록과 먼 블록이다(둘 다 내준 적이 없다).
            CHECK(allocator.Deallocate(next).Error() == ErrorCode::NotFound);
            CHECK(allocator.Deallocate(next + 100 * block).Error() == ErrorCode::NotFound);
            CHECK(SameStats(allocator.Stats(), before));

            void* later[Later]{};
            for (uint32 index = 0; index < Later; ++index) {
                auto result = allocator.Allocate(Request, 16);
                REQUIRE(result);
                later[index] = result.Value();
                CHECK(later[index] != first.Value());
                for (uint32 earlier = 0; earlier < index; ++earlier) {
                    REQUIRE(later[index] != later[earlier]);
                }

                FillPattern(later[index], Request, static_cast<uint8>(index));
            }

            for (uint32 index = 0; index < Later; ++index) {
                CHECK(CheckPattern(later[index], Request, static_cast<uint8>(index)));
                REQUIRE(allocator.Deallocate(later[index]));
            }

            // 마지막 블록을 반환하면 slab이 비어 보관소로 간다. 다음 round는 그 slab을 다시 쓴다:
            // 옛 표지가 남아 있어도 다시 내준 블록은 정상으로 반환된다(내줄 때 표지를 지운다).
            REQUIRE(allocator.Deallocate(first.Value()));
            CHECK(allocator.Stats().liveAllocations == 0);
            CHECK(allocator.Usage().retainedSlabs == 1);
            CHECK(BinnedAllocatorTestAccess::SlabTableEntry(allocator, first.Value()) == 0);
            CHECK(allocator.Deallocate(first.Value()).Error() == ErrorCode::NotFound);
        }

        // 보관한 slab을 다른 등급이 다시 쓴다(옛 등급의 표지·경계가 남아 있다).
        constexpr usize OtherRequest = 4000;
        constexpr uint32 OtherCount = 12;
        void* others[OtherCount]{};
        for (uint32 index = 0; index < OtherCount; ++index) {
            auto result = allocator.Allocate(OtherRequest, 16);
            REQUIRE(result);
            others[index] = result.Value();
            FillPattern(others[index], OtherRequest, static_cast<uint8>(index + 9));
        }

        CHECK(allocator.Usage().retainedSlabs == 0);
        // 옛 등급의 블록 경계는 새 등급의 경계가 아니다.
        CHECK(allocator.Deallocate(static_cast<uint8*>(others[0]) + block).Error() ==
              ErrorCode::NotFound);
        for (uint32 index = 0; index < OtherCount; ++index) {
            CHECK(CheckPattern(others[index], OtherRequest, static_cast<uint8>(index + 9)));
            REQUIRE(allocator.Deallocate(others[index]));
        }

        CHECK(allocator.Stats().liveAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 빈 slab은 한도까지 commit한 채 보관하고 넘으면 decommit한다. 풀린 slab은 표가 0이고, Trim이
// 보관분을 모두 decommit하며, decommit한 slab은 낮은 번호부터 다시 쓴다.
TEST(CoreBinnedEmptySlabsAreRetainedThenDecommitted) {
    SystemAllocator system;
    {
        BinnedAllocatorConfig config;
        config.reserveBytes = 16 * MiB;
        config.retainedEmptySlabs = 2;
        BinnedAllocator allocator(system, config);
        CHECK(allocator.ReservedBytes() == 16 * MiB);
        constexpr usize Request = BinnedAllocator::MaxSmallBytes;
        const uint32 perSlab = BinnedAllocatorTestAccess::ClassCapacity(
            BinnedAllocatorTestAccess::ClassOf(Request, 16));
        constexpr uint32 Slabs = 5;
        constexpr uint32 MaxBlocks = 64;
        const uint32 count = perSlab * Slabs;
        REQUIRE(count <= MaxBlocks);
        void* blocks[MaxBlocks]{};
        for (uint32 index = 0; index < count; ++index) {
            auto result = allocator.Allocate(Request, 16);
            REQUIRE(result);
            blocks[index] = result.Value();
            FillPattern(blocks[index], Request, static_cast<uint8>(index));
        }

        CHECK(allocator.Usage().committedBytes == Slabs * SlabBytes);
        CHECK(allocator.Usage().retainedSlabs == 0);
        // 쓰는 중에는 Trim이 아무것도 풀지 않는다.
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == Slabs * SlabBytes);
        for (uint32 index = 0; index < count; ++index) {
            CHECK(CheckPattern(blocks[index], Request, static_cast<uint8>(index)));
            REQUIRE(allocator.Deallocate(blocks[index]));
        }

        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Usage().retainedSlabs == 2);
        CHECK(allocator.Usage().committedBytes == 2 * SlabBytes);
        for (uint32 index = 0; index < count; ++index) {
            REQUIRE(BinnedAllocatorTestAccess::SlabTableEntry(allocator, blocks[index]) == 0);
            // 풀린 slab(보관·decommit)의 주소는 읽지 않고 NotFound다.
            REQUIRE(allocator.Deallocate(blocks[index]).Error() == ErrorCode::NotFound);
        }

        allocator.Trim();
        CHECK(allocator.Usage().retainedSlabs == 0);
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(allocator.Deallocate(blocks[0]).Error() == ErrorCode::NotFound);

        // decommit한 slab을 다시 commit해 쓴다: 가장 낮은 번호(처음 쓴 slab)부터다.
        uptr lowest = reinterpret_cast<uptr>(blocks[0]);
        for (uint32 index = 1; index < count; ++index) {
            const uptr value = reinterpret_cast<uptr>(blocks[index]);
            lowest = value < lowest ? value : lowest;
        }

        auto again = allocator.Allocate(40, 16);
        REQUIRE(again);
        CHECK((reinterpret_cast<uptr>(again.Value()) & ~(SlabBytes - 1)) ==
              (lowest & ~(SlabBytes - 1)));
        FillPattern(again.Value(), 40, 77);
        CHECK(allocator.Usage().committedBytes == SlabBytes);
        // 다시 commit한 slab에서 뗀 적 없는 블록은 소유가 아니다.
        CHECK(allocator.Deallocate(static_cast<uint8*>(again.Value()) + BlockBytesOf(40, 16))
                  .Error() == ErrorCode::NotFound);
        CHECK(CheckPattern(again.Value(), 40, 77));
        REQUIRE(allocator.Deallocate(again.Value()));
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    {
        // 한도 0이면 비는 즉시 decommit한다.
        BinnedAllocatorConfig config;
        config.reserveBytes = MiB;
        config.retainedEmptySlabs = 0;
        BinnedAllocator allocator(system, config);
        auto block = allocator.Allocate(64, 16);
        REQUIRE(block);
        CHECK(allocator.Usage().committedBytes == SlabBytes);
        REQUIRE(allocator.Deallocate(block.Value()));
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(allocator.Usage().retainedSlabs == 0);
        CHECK(BinnedAllocatorTestAccess::SlabTableEntry(allocator, block.Value()) == 0);
        CHECK(allocator.Deallocate(block.Value()).Error() == ErrorCode::NotFound);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 예약 실패(18.4절, 검토 F4): 절반씩 줄여 MinReserveBytes까지 다시 시도하고, 끝내 못 하면 예약 없이
// 열어 모든 요청이 상위 allocator로 간다. 주입은 인스턴스 하나에만 적용된다.
TEST(CoreBinnedReserveFailureShrinksThenRunsWithoutRange) {
    SystemAllocator system;
    {
        constexpr usize Requested = 4 * BinnedAllocator::MinReserveBytes;
        BinnedAllocatorConfig config;
        config.reserveBytes = Requested;
        {
            BinnedAllocatorTestAccess::FailNextReserveAttempts(1);
            BinnedAllocator shrunk(system, config);
            CHECK(shrunk.ReservedBytes() == Requested / 2);
            CHECK(shrunk.Usage().reservedBytes == Requested / 2);
            auto block = shrunk.Allocate(100, 16);
            REQUIRE(block);
            CHECK(InsideRange(shrunk, block.Value()));
            REQUIRE(shrunk.Deallocate(block.Value()));
            // 주입은 한 인스턴스에만 적용된다.
            BinnedAllocator normal(system, config);
            CHECK(normal.ReservedBytes() == Requested);
        }

        {
            BinnedAllocatorTestAccess::FailNextReserveAttempts(2);
            BinnedAllocator shrunk(system, config);
            CHECK(shrunk.ReservedBytes() == BinnedAllocator::MinReserveBytes);
        }

        {
            // 하한까지 모두 실패하면 예약 없이 연다(시도는 4배·2배·하한 세 번이다).
            BinnedAllocatorTestAccess::FailNextReserveAttempts(3);
            BinnedAllocator unreserved(system, config);
            CHECK(unreserved.ReservedBytes() == 0);
        }

        {
            // 설정이 하한보다 작으면 그 값 한 번만 시도한다.
            BinnedAllocatorConfig small;
            small.reserveBytes = MiB;
            BinnedAllocatorTestAccess::FailNextReserveAttempts(1);
            BinnedAllocator unreserved(system, small);
            CHECK(unreserved.ReservedBytes() == 0);
            BinnedAllocator normal(system, small);
            CHECK(normal.ReservedBytes() == MiB);
        }

        {
            // slab 표를 받지 못해도 예약 없이 연다.
            BinnedAllocatorTestAccess::FailNextSlabTableAllocation();
            BinnedAllocator unreserved(system, config);
            CHECK(unreserved.ReservedBytes() == 0);
        }

        BinnedAllocatorTestAccess::FailNextReserveAttempts(
            BinnedAllocatorTestAccess::AllReserveAttempts);
        BinnedAllocator allocator(system, config);
        BinnedAllocator other(system, config);
        CHECK(allocator.ReservedBytes() == 0);
        CHECK(allocator.Usage().committedBytes == 0);
        // 모든 요청이 큰 경로로 가고 계약은 그대로다.
        constexpr usize Sizes[] = {1, 16, 100, 4096, 8192, 9000};
        constexpr usize SizeCount = sizeof(Sizes) / sizeof(Sizes[0]);
        constexpr usize SmallCount = 5;
        void* blocks[SizeCount]{};
        uint64 expectedBytes = 0;
        for (usize index = 0; index < SizeCount; ++index) {
            auto result = allocator.Allocate(Sizes[index], index % 2 == 0 ? 1 : 64);
            REQUIRE(result);
            blocks[index] = result.Value();
            // 작은 요청의 정렬 보장(16 이상)은 큰 경로에서도 같다.
            CHECK(reinterpret_cast<uptr>(blocks[index]) % (index % 2 == 0 ? 16 : 64) == 0);
            CHECK(!InsideRange(allocator, blocks[index]));
            FillPattern(blocks[index], Sizes[index], static_cast<uint8>(index));
            expectedBytes += Sizes[index];
        }

        CHECK(allocator.Stats().liveAllocations == SizeCount);
        CHECK(allocator.Stats().liveBytes == expectedBytes);
        CHECK(allocator.Usage().largeAllocations == SizeCount);
        CHECK(allocator.Usage().fallbackAllocations == SmallCount);
        CHECK(system.Stats().liveAllocations == SizeCount);
        int local = 0;
        CHECK(allocator.Deallocate(&local).Error() == ErrorCode::NotFound);
        CHECK(allocator.Deallocate(static_cast<uint8*>(blocks[0]) + 16).Error() ==
              ErrorCode::NotFound);
        auto foreign = other.Allocate(64, 16);
        REQUIRE(foreign);
        CHECK(allocator.Deallocate(foreign.Value()).Error() == ErrorCode::NotFound);
        REQUIRE(other.Deallocate(foreign.Value()));
        for (usize index = 0; index < SizeCount; ++index) {
            CHECK(CheckPattern(blocks[index], Sizes[index], static_cast<uint8>(index)));
            REQUIRE(allocator.Deallocate(blocks[index]));
            CHECK(allocator.Deallocate(blocks[index]).Error() == ErrorCode::NotFound);
        }

        allocator.Trim();
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
        CHECK(allocator.Stats().totalAllocations == SizeCount);
    }

    {
        // 큰 경로로 간 작은 요청도 정렬 16 이상을 지킨다(상위가 요청한 정렬만 딱 지켜도). 큰 요청은
        // 요청한 정렬 그대로 상위에 넘긴다.
        MinimalAlignmentBacking minimal(system);
        BinnedAllocatorTestAccess::FailNextReserveAttempts(
            BinnedAllocatorTestAccess::AllReserveAttempts);
        BinnedAllocator allocator(minimal);
        REQUIRE(allocator.ReservedBytes() == 0);
        auto small = allocator.Allocate(100, 1);
        auto wide = allocator.Allocate(100, 64);
        auto large = allocator.Allocate(BinnedAllocator::MaxSmallBytes + 1, 8);
        REQUIRE(small);
        REQUIRE(wide);
        REQUIRE(large);
        CHECK(reinterpret_cast<uptr>(small.Value()) % 16 == 0);
        CHECK(reinterpret_cast<uptr>(wide.Value()) % 64 == 0);
        CHECK(reinterpret_cast<uptr>(large.Value()) % 8 == 0);
        CHECK(reinterpret_cast<uptr>(large.Value()) % 16 != 0);
        REQUIRE(allocator.Deallocate(small.Value()));
        REQUIRE(allocator.Deallocate(wide.Value()));
        REQUIRE(allocator.Deallocate(large.Value()));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 예약 범위가 다 차거나 slab commit이 실패한 작은 요청은 실패가 아니라 큰 경로로 간다.
TEST(CoreBinnedExhaustedRangeAndCommitFailureFallBack) {
    SystemAllocator system;
    {
        constexpr uint32 Slabs = 2;
        BinnedAllocatorConfig config;
        config.reserveBytes = Slabs * SlabBytes;
        BinnedAllocator allocator(system, config);
        REQUIRE(allocator.ReservedBytes() == Slabs * SlabBytes);
        constexpr usize Request = BinnedAllocator::MaxSmallBytes;
        const uint32 perSlab = BinnedAllocatorTestAccess::ClassCapacity(
            BinnedAllocatorTestAccess::ClassOf(Request, 16));
        constexpr uint32 MaxBlocks = 32;
        constexpr uint32 Extra = 3;
        const uint32 inRange = perSlab * Slabs;
        REQUIRE(inRange + Extra <= MaxBlocks);
        void* blocks[MaxBlocks]{};
        for (uint32 index = 0; index < inRange + Extra; ++index) {
            auto result = allocator.Allocate(Request, 16);
            REQUIRE(result);
            blocks[index] = result.Value();
            CHECK(InsideRange(allocator, blocks[index]) == (index < inRange));
            FillPattern(blocks[index], Request, static_cast<uint8>(index));
        }

        CHECK(allocator.Usage().fallbackAllocations == Extra);
        CHECK(allocator.Usage().largeAllocations == Extra);
        CHECK(allocator.Stats().liveAllocations == inRange + Extra);
        // 다른 등급도 slab을 얻지 못해 큰 경로로 간다.
        auto other = allocator.Allocate(24, 16);
        REQUIRE(other);
        CHECK(!InsideRange(allocator, other.Value()));
        CHECK(allocator.Usage().fallbackAllocations == Extra + 1);
        // 범위 안 블록을 반환하면 그 등급은 다시 slab에서 받는다.
        REQUIRE(allocator.Deallocate(blocks[0]));
        auto reused = allocator.Allocate(Request, 16);
        REQUIRE(reused);
        CHECK(reused.Value() == blocks[0]);
        CHECK(allocator.Usage().fallbackAllocations == Extra + 1);
        FillPattern(blocks[0], Request, 0);
        for (uint32 index = 0; index < inRange + Extra; ++index) {
            CHECK(CheckPattern(blocks[index], Request, static_cast<uint8>(index)));
            REQUIRE(allocator.Deallocate(blocks[index]));
        }

        REQUIRE(allocator.Deallocate(other.Value()));
        CHECK(allocator.Stats().liveAllocations == 0);
    }

    {
        BinnedAllocatorConfig config;
        config.reserveBytes = MiB;
        BinnedAllocator allocator(system, config);
        BinnedAllocatorTestAccess::FailSlabCommitOnAttempt(allocator, 1);
        auto fallback = allocator.Allocate(200, 32);
        REQUIRE(fallback);
        CHECK(!InsideRange(allocator, fallback.Value()));
        CHECK(reinterpret_cast<uptr>(fallback.Value()) % 32 == 0);
        CHECK(allocator.Usage().fallbackAllocations == 1);
        CHECK(allocator.Usage().committedBytes == 0);
        // 다음 commit은 성공한다(같은 slab 번호를 다시 시도한다).
        auto normal = allocator.Allocate(200, 32);
        REQUIRE(normal);
        CHECK(InsideRange(allocator, normal.Value()));
        CHECK(allocator.Usage().committedBytes == SlabBytes);
        CHECK(allocator.Stats().liveAllocations == 2);
        REQUIRE(allocator.Deallocate(fallback.Value()));
        REQUIRE(allocator.Deallocate(normal.Value()));
        CHECK(allocator.Stats().liveAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// decommit이 거절되면(닫힌 region) slab을 commit한 채 보관해 다시 쓰고, OS 실패면 그 slab을 다시
// 쓰지 않는다.
TEST(CoreBinnedDecommitFaultsKeepOrAbandonTheSlab) {
    SystemAllocator system;
    {
        BinnedAllocatorConfig config;
        config.reserveBytes = MiB;
        config.retainedEmptySlabs = 0;
        BinnedAllocator allocator(system, config);
        BinnedAllocatorTestAccess::SetSlabDecommitFault(
            allocator, BinnedAllocatorTestAccess::DecommitFault::Refused);
        auto first = allocator.Allocate(64, 16);
        REQUIRE(first);
        REQUIRE(allocator.Deallocate(first.Value()));
        // 한도가 0이어도 보관한다(commit된 채다).
        CHECK(allocator.Usage().retainedSlabs == 1);
        CHECK(allocator.Usage().committedBytes == SlabBytes);
        CHECK(BinnedAllocatorTestAccess::SlabTableEntry(allocator, first.Value()) == 0);
        allocator.Trim();
        CHECK(allocator.Usage().retainedSlabs == 1);
        auto second = allocator.Allocate(64, 16);
        REQUIRE(second);
        CHECK(second.Value() == first.Value());
        CHECK(allocator.Usage().retainedSlabs == 0);
        FillPattern(second.Value(), 64, 1);

        BinnedAllocatorTestAccess::SetSlabDecommitFault(
            allocator, BinnedAllocatorTestAccess::DecommitFault::Lost);
        CHECK(CheckPattern(second.Value(), 64, 1));
        REQUIRE(allocator.Deallocate(second.Value()));
        CHECK(allocator.Usage().retainedSlabs == 0);
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(BinnedAllocatorTestAccess::SlabTableEntry(allocator, second.Value()) == 0);
        CHECK(allocator.Deallocate(second.Value()).Error() == ErrorCode::NotFound);

        BinnedAllocatorTestAccess::SetSlabDecommitFault(
            allocator, BinnedAllocatorTestAccess::DecommitFault::None);
        // 잃은 slab은 다시 쓰지 않는다: 다음 slab에서 받는다.
        auto third = allocator.Allocate(64, 16);
        REQUIRE(third);
        CHECK((reinterpret_cast<uptr>(third.Value()) & ~(SlabBytes - 1)) !=
              (reinterpret_cast<uptr>(second.Value()) & ~(SlabBytes - 1)));
        FillPattern(third.Value(), 64, 2);
        CHECK(CheckPattern(third.Value(), 64, 2));
        REQUIRE(allocator.Deallocate(third.Value()));
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 큰 경로의 실패: 상위 allocator 실패와 원장 늘리기 실패는 통계·상위 상태를 바꾸지 않고, 상위가
// 반환을 거부하면 할당이 살아 있는 채로 남는다. 상위 호출은 잠금 밖이라 다시 불러도 된다.
TEST(CoreBinnedLargePathFailuresRollBack) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    FreeFailureAllocator freeFailing(failing);
    ReentrantBacking reentrant(freeFailing);
    {
        BinnedAllocator allocator(reentrant);
        constexpr usize LargeBytes = 20000;
        auto kept = allocator.Allocate(LargeBytes, 16);
        REQUIRE(kept);
        FillPattern(kept.Value(), LargeBytes, 4);
        const AllocationStats before = allocator.Stats();
        const uint64 systemLive = system.Stats().liveAllocations;

        failing.FailOnAttempt(1);
        CHECK(allocator.Allocate(LargeBytes, 16).Error() == ErrorCode::OutOfMemory);
        failing.DisableFailure();
        CHECK(SameStats(allocator.Stats(), before));
        CHECK(system.Stats().liveAllocations == systemLive);

        // 원장 표를 늘리지 못하면 OutOfMemory이고 상위 블록을 돌려준다.
        BinnedAllocatorTestAccess::FailLedgerStorageOnAttempt(allocator, 1);
        // 표는 몇십 개 안에서 늘어난다(실패하지 않으면 여기서 멈춘다).
        constexpr uint32 Limit = 4096;
        Array<void*> blocks(system);
        REQUIRE(blocks.TryReserve(Limit));
        ErrorCode failure = ErrorCode::None;
        for (uint32 index = 0; index < Limit; ++index) {
            auto result = allocator.Allocate(BinnedAllocator::MaxSmallBytes + 1, 16);
            if (!result) {
                failure = result.Error();
                break;
            }

            REQUIRE(blocks.TryPushBack(result.Value()));
        }

        CHECK(failure == ErrorCode::OutOfMemory);
        BinnedAllocatorTestAccess::FailLedgerStorageOnAttempt(allocator, 0);
        CHECK(allocator.Stats().liveAllocations == before.liveAllocations + blocks.Size());
        CHECK(allocator.Usage().largeAllocations == 1 + blocks.Size());
        // 실패한 할당의 상위 블록은 남지 않았다(blocks 배열 저장소 하나가 더 있다).
        CHECK(system.Stats().liveAllocations == systemLive + 1 + blocks.Size());
        for (void* block : blocks) {
            REQUIRE(allocator.Deallocate(block));
        }

        CHECK(
            SameStats(allocator.Stats(), AllocationStats{before.liveAllocations, before.liveBytes,
                                                         before.totalAllocations + blocks.Size()}));

        // 상위가 반환을 거부하면 그 오류를 돌려주고 할당은 살아 있다.
        freeFailing.failFree = true;
        const AllocationStats live = allocator.Stats();
        CHECK(allocator.Deallocate(kept.Value()).Error() == ErrorCode::Busy);
        CHECK(SameStats(allocator.Stats(), live));
        CHECK(allocator.Usage().largeAllocations == 1);
        CHECK(CheckPattern(kept.Value(), LargeBytes, 4));
        freeFailing.failFree = false;

        // 상위 호출 안에서 같은 allocator를 다시 불러도 된다(잠금 밖에서 부른다).
        reentrant.target = &allocator;
        auto nested = allocator.Allocate(LargeBytes, 16);
        REQUIRE(nested);
        REQUIRE(allocator.Deallocate(nested.Value()));
        reentrant.target = nullptr;
        CHECK(reentrant.nestedSuccesses == 2);

        REQUIRE(allocator.Deallocate(kept.Value()));
        CHECK(allocator.Deallocate(kept.Value()).Error() == ErrorCode::NotFound);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 무작위 할당·반환을 그림자 표와 대조한다: 내용 보존(겹침 없음), 통계 일치, 끝에 Trim 뒤 commit된
// slab 0.
TEST(CoreBinnedRandomChurnMatchesShadowModel) {
    SystemAllocator system;
    SystemAllocator shadowMemory;
    {
        struct Shadow {
            void* address = nullptr;
            usize bytes = 0;
            usize accounted = 0;
            uint8 seed = 0;
        };

        BinnedAllocatorConfig config;
        config.reserveBytes = BinnedAllocator::MinReserveBytes;
        config.retainedEmptySlabs = 4;
        BinnedAllocator allocator(system, config);
        constexpr uint32 Slots = 3000;
        constexpr uint32 Operations = 120000;
        constexpr usize MaxRequest = 12000;
        // 요청의 대부분은 작은 크기다(여덟에 일곱).
        constexpr uint32 SmallShare = 8;
        constexpr usize SmallRequest = 512;
        Array<Shadow> shadow(shadowMemory);
        REQUIRE(shadow.TryResize(Slots));
        BinnedRng rng{0x42494e4eull};
        uint64 liveCount = 0;
        uint64 liveBytes = 0;
        uint64 total = 0;
        for (uint32 operation = 0; operation < Operations; ++operation) {
            Shadow& slot = shadow[static_cast<uint32>(rng.Next() % Slots)];
            if (slot.address != nullptr) {
                REQUIRE(CheckPattern(slot.address, slot.bytes, slot.seed));
                REQUIRE(allocator.Deallocate(slot.address));
                --liveCount;
                liveBytes -= slot.accounted;
                slot = Shadow{};
                continue;
            }

            const bool small = rng.Next() % SmallShare != 0;
            const usize bytes =
                1 + static_cast<usize>(rng.Next() % (small ? SmallRequest : MaxRequest));
            const usize alignment = usize{1} << (rng.Next() % 8);
            auto result = allocator.Allocate(bytes, alignment);
            REQUIRE(result);
            REQUIRE(reinterpret_cast<uptr>(result.Value()) % alignment == 0);
            const bool inRange = bytes <= BinnedAllocator::MaxSmallBytes &&
                                 alignment <= BinnedAllocator::MaxSmallAlignment;
            REQUIRE(InsideRange(allocator, result.Value()) == inRange);
            slot.address = result.Value();
            slot.bytes = bytes;
            slot.accounted = inRange ? BlockBytesOf(bytes, alignment) : bytes;
            slot.seed = static_cast<uint8>(rng.Next());
            FillPattern(slot.address, slot.bytes, slot.seed);
            ++liveCount;
            ++total;
            liveBytes += slot.accounted;
            if (operation % 1024 == 0) {
                const AllocationStats stats = allocator.Stats();
                REQUIRE(stats.liveAllocations == liveCount);
                REQUIRE(stats.liveBytes == liveBytes);
                REQUIRE(stats.totalAllocations == total);
            }
        }

        const AllocationStats stats = allocator.Stats();
        CHECK(stats.liveAllocations == liveCount);
        CHECK(stats.liveBytes == liveBytes);
        CHECK(stats.totalAllocations == total);
        CHECK(allocator.Usage().fallbackAllocations == 0);
        for (Shadow& slot : shadow) {
            if (slot.address != nullptr) {
                REQUIRE(CheckPattern(slot.address, slot.bytes, slot.seed));
                REQUIRE(allocator.Deallocate(slot.address));
            }
        }

        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
        CHECK(allocator.Usage().largeAllocations == 0);
        CHECK(allocator.Usage().retainedSlabs <= config.retainedEmptySlabs);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(allocator.Usage().retainedSlabs == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
    CHECK(shadowMemory.Stats().liveAllocations == 0);
}

// 여러 스레드가 함께 할당·반환해도 내용이 보존되고 통계가 맞는다.
TEST(CoreBinnedConcurrentAllocationStress) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        constexpr uint32 Workers = 4;
        StartGate gate;
        StressWork work[Workers]{{&allocator, &gate, 1},
                                 {&allocator, &gate, 2},
                                 {&allocator, &gate, 3},
                                 {&allocator, &gate, 4}};
        platform::Thread threads[Workers];
        for (uint32 index = 0; index < Workers; ++index) {
            StartWorker(threads[index], RunStressWork, &work[index]);
        }

        gate.OpenWhenArrived(Workers);
        for (uint32 index = 0; index < Workers; ++index) {
            JoinWorker(threads[index]);
            CHECK(work[index].failures == 0);
        }

        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
        CHECK(allocator.Stats().totalAllocations == uint64{Workers} * StressIterations);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 한 스레드가 할당한 블록을 다른 스레드가 반환한다. 같은 주소를 두 스레드가 함께 반환하면 한쪽만
// 성공한다.
TEST(CoreBinnedCrossThreadAndConcurrentDuplicateReturn) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        constexpr usize Count = 512;
        void* addresses[Count]{};
        for (usize index = 0; index < Count; ++index) {
            auto result = allocator.Allocate(index % 2 == 0 ? 40 : 9000, 16);
            REQUIRE(result);
            addresses[index] = result.Value();
        }

        ReturnWork work[2]{{&allocator, addresses, Count / 2},
                           {&allocator, addresses + Count / 2, Count / 2}};
        platform::Thread threads[2];
        for (usize index = 0; index < 2; ++index) {
            StartWorker(threads[index], RunReturnWork, &work[index]);
        }

        for (usize index = 0; index < 2; ++index) {
            JoinWorker(threads[index]);
            CHECK(work[index].failures == 0);
        }

        CHECK(allocator.Stats().liveAllocations == 0);
        constexpr uint32 Rounds = 64;
        for (uint32 round = 0; round < Rounds; ++round) {
            // 같은 slab에 살아 있는 블록을 남겨 둔다(중복 반환이 slab 풀기와 겹치지 않게 — 18.4절
            // 한계 2).
            auto anchor = allocator.Allocate(25, 8);
            auto result = allocator.Allocate(25, 8);
            REQUIRE(anchor);
            REQUIRE(result);
            StartGate gate;
            DuplicateWork duplicates[2]{{&allocator, &gate, result.Value()},
                                        {&allocator, &gate, result.Value()}};
            for (usize index = 0; index < 2; ++index) {
                StartWorker(threads[index], RunDuplicateWork, &duplicates[index]);
            }

            gate.OpenWhenArrived(2);
            for (auto& thread : threads) {
                JoinWorker(thread);
            }

            CHECK((duplicates[0].result == ErrorCode::None &&
                   duplicates[1].result == ErrorCode::NotFound) ||
                  (duplicates[1].result == ErrorCode::None &&
                   duplicates[0].result == ErrorCode::NotFound));
            CHECK(allocator.Stats().liveAllocations == 1);
            REQUIRE(allocator.Deallocate(anchor.Value()));
        }

        CHECK(allocator.Stats().liveAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}
} // namespace binned_allocator_tests_file
