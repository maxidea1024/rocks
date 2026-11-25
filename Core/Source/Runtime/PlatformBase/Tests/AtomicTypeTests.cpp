#include <Base/TypeTraits.h>
#include <Platform/Atomics.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace atomic_type_tests_file {

using namespace rocks::platform;

// Atomic<T>·AtomicRef<T>·보조 연산·SpinBackoff(Docs/Design/Synchronization 11절)를 본다. 단일
// 스레드 의미(반환값·경계·감김)와 여러 스레드에서 잃는 갱신·겹치는 번호가 없는지를 결정적으로
// 단언한다.
namespace {

enum class Mode : uint16 {
    Off,
    On
};

// 상수 초기화되고 파괴자가 없어 constinit 전역으로 둘 수 있다.
constinit Atomic<uint64> globalCounter;
constinit Atomic<Mode> globalMode(Mode::On);

static_assert(sizeof(Atomic<uint8>) == 1 && alignof(Atomic<uint8>) == 1);
static_assert(sizeof(Atomic<uint64>) == 8 && alignof(Atomic<uint64>) == 8);
static_assert(sizeof(Atomic<void*>) == sizeof(void*) && alignof(Atomic<void*>) == alignof(void*));
static_assert(IsTriviallyDestructibleV<Atomic<uint32>> && IsStandardLayoutV<Atomic<uint32>>);
static_assert(sizeof(AtomicRef<uint64>) == sizeof(void*));

// 8 byte 값이 4 byte 정렬 구조체 안에서도 자연 정렬된다.
struct Packed {
    uint32 head;
    Atomic<uint64> value;
};

static_assert(alignof(Packed) == 8 && sizeof(Packed) == 16);

constexpr uint32 Workers = 4;
constexpr uint32 NumbersPerWorker = 5000;

struct NumberContext {
    Atomic<uint32> source;
    Atomic<uint64> maximum;
    uint32 taken[Workers][NumbersPerWorker] = {};
    Atomic<uint32> nextWorker;
};

void TakeNumbers(void* raw) noexcept {
    auto& context = *static_cast<NumberContext*>(raw);
    const uint32 worker = context.nextWorker.FetchAdd<MemoryOrder::Relaxed>(1);
    for (uint32 index = 0; index < NumbersPerWorker; ++index) {
        uint32 number = 0;
        if (!context.source.TakeUniqueNumber<MemoryOrder::Relaxed>(number)) {
            return;
        }

        context.taken[worker][index] = number;
        context.maximum.FetchMax<MemoryOrder::Relaxed>(number);
    }
}

} // namespace

TEST(AtomicType_ConstantInitializedGlobals) {
    CHECK(globalMode.Load<MemoryOrder::Relaxed>() == Mode::On);
    globalMode.Store<MemoryOrder::Relaxed>(Mode::Off);
    CHECK(globalMode.Exchange<MemoryOrder::AcquireRelease>(Mode::On) == Mode::Off);
    CHECK(globalCounter.FetchAdd<MemoryOrder::Relaxed>(2) == 0);
    CHECK(globalCounter.FetchSub<MemoryOrder::Relaxed>(2) == 2);
}

TEST(AtomicType_MemberSemantics) {
    Atomic<uint32> value(0xF0);
    CHECK(value.FetchOr<MemoryOrder::Relaxed>(0x0F) == 0xF0);
    CHECK(value.FetchAnd<MemoryOrder::Relaxed>(0x3C) == 0xFF);
    CHECK(value.FetchXor<MemoryOrder::Relaxed>(0xFF) == 0x3C);
    CHECK(value.Load<MemoryOrder::SequentiallyConsistent>() == 0xC3);

    // 실패한 CAS는 현재 값을 expected에 쓴다.
    uint32 expected = 1;
    CHECK(!value.CompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(expected,
                                                                                          2));
    CHECK(expected == 0xC3);
    CHECK(value.CompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(expected,
                                                                                         2));
    CHECK(value.Load<MemoryOrder::Acquire>() == 2);

    // 부호 없는 산술은 2^N으로 감싼다.
    Atomic<uint8> small(255);
    CHECK(small.FetchAdd<MemoryOrder::Relaxed>(1) == 255 &&
          small.Load<MemoryOrder::Relaxed>() == 0);

    int target = 0;
    Atomic<int*> pointer;
    CHECK(pointer.Load<MemoryOrder::Relaxed>() == nullptr);
    pointer.Store<MemoryOrder::Release>(&target);
    CHECK(pointer.Load<MemoryOrder::Acquire>() == &target);

    Atomic<bool> flag;
    CHECK(!flag.Exchange<MemoryOrder::Relaxed>(true) && flag.Load<MemoryOrder::Relaxed>());
}

TEST(AtomicType_HelperOperations) {
    // FetchMax: 클 때만 바꾸고 이전 값을 돌려준다.
    Atomic<uint32> maximum(10);
    CHECK(maximum.FetchMax<MemoryOrder::Relaxed>(5) == 10 &&
          maximum.Load<MemoryOrder::Relaxed>() == 10);
    CHECK(maximum.FetchMax<MemoryOrder::Relaxed>(20) == 10 &&
          maximum.Load<MemoryOrder::Relaxed>() == 20);

    // TryFetchAddBounded: 결과가 한도 이하일 때만 더한다. 넘침(감김)도 거부한다.
    Atomic<uint64> reserved(90);
    uint64 start = 7;
    CHECK(reserved.TryFetchAddBounded<MemoryOrder::Relaxed>(10, 100, start) && start == 90);
    CHECK(!reserved.TryFetchAddBounded<MemoryOrder::Relaxed>(1, 100, start) && start == 90);
    CHECK(reserved.Load<MemoryOrder::Relaxed>() == 100);
    Atomic<uint64> nearMax(NumericLimits<uint64>::Max() - 1);
    CHECK(
        !nearMax.TryFetchAddBounded<MemoryOrder::Relaxed>(2, NumericLimits<uint64>::Max(), start));
    Atomic<uint64> overLimit(150);
    CHECK(!overLimit.TryFetchAddBounded<MemoryOrder::Relaxed>(0, 100, start));

    // FetchAddSaturating: 넘으면 한도에서 멈춘다. 이미 한도 이상이면 바꾸지 않는다.
    Atomic<uint32> saturating(95);
    CHECK(saturating.FetchAddSaturating<MemoryOrder::Relaxed>(10, 100) == 95);
    CHECK(saturating.Load<MemoryOrder::Relaxed>() == 100);
    CHECK(saturating.FetchAddSaturating<MemoryOrder::Relaxed>(1, 100) == 100);
    Atomic<uint32> beyond(120);
    CHECK(beyond.FetchAddSaturating<MemoryOrder::Relaxed>(1, 100) == 120);
    CHECK(beyond.Load<MemoryOrder::Relaxed>() == 120);

    // TryIncrementIfNonZero: 0과 최댓값에서는 올리지 않는다.
    Atomic<uint32> strong;
    CHECK(!strong.TryIncrementIfNonZero<MemoryOrder::AcquireRelease>());
    strong.Store<MemoryOrder::Relaxed>(1);
    CHECK(strong.TryIncrementIfNonZero<MemoryOrder::AcquireRelease>());
    CHECK(strong.Load<MemoryOrder::Relaxed>() == 2);
    strong.Store<MemoryOrder::Relaxed>(NumericLimits<uint32>::Max());
    CHECK(!strong.TryIncrementIfNonZero<MemoryOrder::AcquireRelease>());

    // TakeUniqueNumber: 1부터 최댓값까지 주고 다 쓰면 false다(0은 주지 않는다).
    Atomic<uint8> numbers;
    uint8 number = 0;
    CHECK(numbers.TakeUniqueNumber<MemoryOrder::Relaxed>(number) && number == 1);
    numbers.Store<MemoryOrder::Relaxed>(254);
    CHECK(numbers.TakeUniqueNumber<MemoryOrder::Relaxed>(number) && number == 255);
    number = 7;
    CHECK(!numbers.TakeUniqueNumber<MemoryOrder::Relaxed>(number) && number == 7);

    // FetchUpdate: 함수 결과를 쓰고 이전 값을 돌려준다.
    Atomic<uint64> bits(3);
    CHECK(bits.FetchUpdate<MemoryOrder::Relaxed>([](uint64 old) noexcept { return old * 5; }) == 3);
    CHECK(bits.Load<MemoryOrder::Relaxed>() == 15);
}

TEST(AtomicType_RefViewsExistingObject) {
    // trivially copyable 칸의 필드를 원자로 본다. 일반 쓰기로 초기화한 뒤 원자 접근으로 옮겨간다.
    struct Cell {
        uint64 sequence;
        uint32 payload;
    };

    static_assert(IsTriviallyCopyableV<Cell>);
    Cell cell{4, 9};
    AtomicRef<uint64> sequence(cell.sequence);
    CHECK(sequence.Load<MemoryOrder::Acquire>() == 4);
    sequence.Store<MemoryOrder::Release>(5);
    CHECK(cell.sequence == 5);
    CHECK(AtomicRef<uint32>(cell.payload).FetchAdd<MemoryOrder::Relaxed>(1) == 9 &&
          cell.payload == 10);
    const AtomicRef<uint64> copy = sequence;
    CHECK(copy.Load<MemoryOrder::Relaxed>() == 5);
    CHECK(AtomicRef<uint64>(cell.sequence).FetchMax<MemoryOrder::Relaxed>(8) == 5 &&
          cell.sequence == 8);
}

TEST(AtomicType_ConcurrentUniqueNumbersAndMaximum) {
    NumberContext context;
    Thread threads[Workers];
    for (Thread& thread : threads) {
        REQUIRE(thread.Start(TakeNumbers, &context) == ThreadStatus::Success);
    }

    for (Thread& thread : threads) {
        REQUIRE(thread.Join() == ThreadStatus::Success);
    }

    // 모든 번호는 1..Workers×N에 정확히 한 번씩 나온다(겹침·빠짐 없음).
    constexpr uint32 Total = Workers * NumbersPerWorker;
    bool seen[Total + 1] = {};
    bool unique = true;
    for (uint32 worker = 0; worker < Workers; ++worker) {
        for (uint32 index = 0; index < NumbersPerWorker; ++index) {
            const uint32 number = context.taken[worker][index];
            unique = unique && number >= 1 && number <= Total && !seen[number];
            if (number <= Total) {
                seen[number] = true;
            }
        }
    }

    CHECK(unique);
    CHECK(context.source.Load<MemoryOrder::Relaxed>() == Total);
    CHECK(context.maximum.Load<MemoryOrder::Relaxed>() == Total);
}

TEST(AtomicType_SpinBackoffPausesThenYields) {
    SpinBackoff backoff;
    for (uint32 round = 0; round < SpinBackoff::PauseLimit; ++round) {
        CHECK(backoff.PauseCount() == round);
        backoff.Pause();
    }

    // PauseLimit 뒤로는 횟수가 멈추고 OS 양보로 넘어간다.
    backoff.Pause();
    backoff.Pause();
    CHECK(backoff.PauseCount() == SpinBackoff::PauseLimit);
    CpuPause();
}

} // namespace atomic_type_tests_file
