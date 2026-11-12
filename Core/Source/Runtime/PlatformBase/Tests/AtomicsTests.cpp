#include <Platform/Atomics.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::platform;

// Platform/Atomics.h의 단일 스레드 의미(반환값·expected 갱신·감싸기)와 여러 스레드 경합에서 잃는
// 갱신이 없는지, Release/Acquire 발행이 데이터를 보이게 하는지 검사한다.
namespace {

enum class Phase : uint8 {
    Idle,
    Running,
    Done
};

struct Counters {
    uint64 total = 0;
    uint32 flags = 0;
    int32 casTotal = 0;
};

constexpr int Workers = 4;
constexpr int Iterations = 100000;

void AddMany(void* context) noexcept {
    auto* counters = static_cast<Counters*>(context);
    for (int index = 0; index < Iterations; ++index) {
        AtomicFetchAdd<MemoryOrder::Relaxed>(&counters->total, 1);
        // CAS 루프로만 증가시키는 계수다. Weak의 거짓 실패도 루프가 흡수한다.
        int32 expected = AtomicLoad<MemoryOrder::Relaxed>(&counters->casTotal);
        while (!AtomicCompareExchangeWeak<MemoryOrder::Relaxed, MemoryOrder::Relaxed>(
            &counters->casTotal, expected, expected + 1)) {
        }
    }
}

struct Publication {
    uint64 payload[4]{};
    uint32 ready = 0;
};

void Publish(void* context) noexcept {
    auto* publication = static_cast<Publication*>(context);
    for (uint64 index = 0; index < 4; ++index) {
        publication->payload[index] = 0x1111111111111111ull * (index + 1);
    }
    AtomicStore<MemoryOrder::Release>(&publication->ready, 1);
}

} // namespace

TEST(Atomics_SingleThreadSemantics) {
    uint32 value = 5;
    CHECK(AtomicLoad(&value) == 5);
    AtomicStore<MemoryOrder::Relaxed>(&value, 7);
    CHECK(AtomicLoad<MemoryOrder::Acquire>(&value) == 7);
    CHECK(AtomicExchange(&value, 9) == 7);
    CHECK(value == 9);

    // Fetch*는 연산 전 값을 반환한다. 부호 없는 정수는 감싼다.
    CHECK(AtomicFetchAdd(&value, 1) == 9 && value == 10);
    CHECK(AtomicFetchSub(&value, 11) == 10 && value == 0xFFFFFFFFu);
    CHECK(AtomicFetchAnd(&value, 0x0F0Fu) == 0xFFFFFFFFu && value == 0x0F0Fu);
    CHECK(AtomicFetchOr(&value, 0xF000u) == 0x0F0Fu && value == 0xFF0Fu);
    CHECK(AtomicFetchXor(&value, 0x00FFu) == 0xFF0Fu && value == 0xFFF0u);

    // Strong CAS: 일치하면 바꾸고, 다르면 현재 값을 expected에 쓴다.
    uint32 expected = 1;
    CHECK(!AtomicCompareExchangeStrong(&value, expected, 2u));
    CHECK(expected == 0xFFF0u && value == 0xFFF0u);
    const bool swapped =
        AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &value, expected, 3u);
    CHECK(swapped);
    CHECK(value == 3u);

    // 부호 있는 정수도 2의 보수로 감싼다.
    int8 small = 127;
    CHECK(AtomicFetchAdd(&small, 1) == 127 && small == -128);

    // 열거형과 포인터는 읽기·쓰기·교환·CAS를 지원한다.
    Phase phase = Phase::Idle;
    CHECK(AtomicExchange(&phase, Phase::Running) == Phase::Idle);
    Phase expectedPhase = Phase::Running;
    CHECK(AtomicCompareExchangeStrong(&phase, expectedPhase, Phase::Done));
    CHECK(AtomicLoad(&phase) == Phase::Done);
    int first = 1;
    int second = 2;
    int* pointer = &first;
    CHECK(AtomicExchange(&pointer, &second) == &first);
    int* expectedPointer = &first;
    CHECK(!AtomicCompareExchangeStrong(&pointer, expectedPointer, &first));
    CHECK(expectedPointer == &second && AtomicLoad(&pointer) == &second);

    uint64 wide = 0xFFFFFFFFull;
    CHECK(AtomicFetchAdd<MemoryOrder::Relaxed>(&wide, 1) == 0xFFFFFFFFull);
    CHECK(wide == 0x100000000ull);
    AtomicThreadFence<MemoryOrder::SequentiallyConsistent>();
}

TEST(Atomics_ConcurrentIncrementsAreNotLost) {
    Counters counters;
    Thread threads[Workers];
    for (Thread& thread : threads) {
        REQUIRE(thread.Start(AddMany, &counters) == ThreadStatus::Success);
    }
    for (Thread& thread : threads) {
        REQUIRE(thread.Join() == ThreadStatus::Success);
    }
    CHECK(counters.total == static_cast<uint64>(Workers) * Iterations);
    CHECK(counters.casTotal == Workers * Iterations);
}

TEST(Atomics_ReleaseStorePublishesPriorWrites) {
    for (int round = 0; round < 64; ++round) {
        Publication publication;
        Thread thread;
        REQUIRE(thread.Start(Publish, &publication) == ThreadStatus::Success);
        while (AtomicLoad<MemoryOrder::Acquire>(&publication.ready) == 0) {
            YieldThread();
        }
        for (uint64 index = 0; index < 4; ++index) {
            CHECK(publication.payload[index] == 0x1111111111111111ull * (index + 1));
        }
        REQUIRE(thread.Join() == ThreadStatus::Success);
    }
}
