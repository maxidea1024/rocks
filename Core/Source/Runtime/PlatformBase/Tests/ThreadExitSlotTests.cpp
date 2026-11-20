#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/OsThread.h>
#include <TestSupport/Test.h>

// platform::ThreadExitSlot: 스레드별 값과 스레드 종료 정리 함수다(Docs/Design/
// GarbageCollectionScale.ko.md 18.4절, 검토 F3).
using namespace rocks;
namespace thread_exit_slot_tests_file {

using namespace rocks::platform;

namespace {

constexpr uint32 WorkerTimeoutMs = 60000;

// 정리 함수가 본 것이다. 값은 이 기록의 주소다.
struct ExitRecord {
    uint32 calls = 0;
    uint64 callbackThread = 0;
    // 정리 함수 안에서 thread_local에 쓴 뒤 다시 읽은 값이다(접근이 되는지 — 계약).
    uint32 localAfterWrite = 0;
    // 정리 함수에 들어올 때 읽은 thread_local 값이다. 단언하지 않는다: 스레드가 두었던 값이 남아
    // 있다는 보장이 없다(macOS는 초기값이다).
    uint32 localOnEntry = 0;
    // 정리 함수 안에서 칸의 값이 이미 지워져 있었는지다.
    bool clearedInside = false;
};

thread_local uint32 localValue = 0;
// 스레드가 thread_local에 두는 값과 정리 함수가 쓰는 값이다.
constexpr uint32 LocalMarker = 0x51075107u;
constexpr uint32 CallbackMarker = 0x0ca11bacu;

// 칸은 프로세스 수명 동안 하나만 만든다(OS 칸을 풀지 않는다).
ThreadExitSlot& TestSlot() noexcept;

void RecordExit(void* value) noexcept {
    auto& record = *static_cast<ExitRecord*>(value);
    AtomicFetchAdd<MemoryOrder::SequentiallyConsistent>(&record.calls, 1);
    record.callbackThread = CurrentThreadId();
    record.clearedInside = TestSlot().Get() == nullptr;
    // trivial한 thread_local은 정리 함수 안에서 읽고 쓸 수 있다(쓰고 읽으면 쓴 값이다).
    record.localOnEntry = localValue;
    localValue = CallbackMarker;
    record.localAfterWrite = localValue;
}

ThreadExitSlot& TestSlot() noexcept {
    static ThreadExitSlot slot(&RecordExit);
    return slot;
}

struct SlotWork {
    ExitRecord* record = nullptr;
    // 스레드가 시작할 때 본 값과 Set 뒤에 본 값이다.
    void* initial = &initial;
    void* afterSet = nullptr;
    uint64 thread = 0;
    bool setSucceeded = false;
    bool clearAgain = false;
};

void RunSlotWork(void* context) noexcept {
    auto& work = *static_cast<SlotWork*>(context);
    work.thread = CurrentThreadId();
    work.initial = TestSlot().Get();
    localValue = LocalMarker;
    if (work.record != nullptr) {
        work.setSucceeded = TestSlot().Set(work.record);
        work.afterSet = TestSlot().Get();
        if (work.clearAgain) {
            work.setSucceeded = TestSlot().Set(nullptr) && work.setSucceeded;
        }
    }
}

void JoinOrFatal(Thread& thread) noexcept {
    if (thread.WaitForCompletion(WorkerTimeoutMs) != WaitResult::Signaled ||
        thread.Join() != ThreadStatus::Success) {
        Fatal("ThreadExitSlot fixture failed to terminate");
    }
}

} // namespace

// 값을 둔 스레드가 끝나면 그 스레드에서 정리 함수가 한 번 불린다(Join이 돌아오기 전에). 값은
// 스레드마다 따로다.
TEST(ThreadExitSlot_CallbackRunsOnceOnTheExitingThread) {
    REQUIRE(TestSlot().IsValid());
    ExitRecord mainMarker;
    REQUIRE(TestSlot().Set(&mainMarker));
    ExitRecord record;
    SlotWork work;
    work.record = &record;
    Thread thread;
    REQUIRE(thread.Start(RunSlotWork, &work) == ThreadStatus::Success);
    JoinOrFatal(thread);
    // 새 스레드는 빈 값에서 시작하고 자기 값만 본다.
    CHECK(work.initial == nullptr);
    CHECK(work.setSucceeded);
    CHECK(work.afterSet == &record);
    CHECK(TestSlot().Get() == &mainMarker);
    // Join이 돌아왔으면 정리 함수는 이미 끝났다.
    CHECK(AtomicLoad(&record.calls) == 1);
    CHECK(record.callbackThread == work.thread);
    CHECK(record.callbackThread != CurrentThreadId());
    // 정리 함수 안에서 그 스레드의 thread_local에 접근할 수 있다. 스레드가 두었던 값이 남아
    // 있는지는 단언하지 않는다(계약 — 플랫폼마다 다르다).
    CHECK(record.localAfterWrite == CallbackMarker);
    // 부르기 전에 그 스레드의 값은 지워져 있다.
    CHECK(record.clearedInside);
    CHECK(mainMarker.calls == 0);
    // 이 스레드가 끝날 때(프로세스 종료) 죽은 지역 변수로 불리지 않게 값을 지운다.
    REQUIRE(TestSlot().Set(nullptr));
    CHECK(TestSlot().Get() == nullptr);
}

// 값을 두지 않았거나 null로 되돌린 스레드에서는 불리지 않는다.
TEST(ThreadExitSlot_NullValueSkipsTheCallback) {
    REQUIRE(TestSlot().IsValid());
    ExitRecord record;
    SlotWork untouched;
    Thread thread;
    REQUIRE(thread.Start(RunSlotWork, &untouched) == ThreadStatus::Success);
    JoinOrFatal(thread);
    CHECK(untouched.initial == nullptr);

    SlotWork cleared;
    cleared.record = &record;
    cleared.clearAgain = true;
    REQUIRE(thread.Start(RunSlotWork, &cleared) == ThreadStatus::Success);
    JoinOrFatal(thread);
    CHECK(cleared.setSucceeded);
    CHECK(cleared.afterSet == &record);
    CHECK(AtomicLoad(&record.calls) == 0);
}

// platform::Thread로 만들지 않은 OS 스레드에서도 불린다.
TEST(ThreadExitSlot_CallbackRunsOnRawOsThreads) {
    REQUIRE(TestSlot().IsValid());
    constexpr uint32 Threads = 3;
    for (uint32 index = 0; index < Threads; ++index) {
        ExitRecord record;
        SlotWork work;
        work.record = &record;
        REQUIRE(test::RunOnOsThread(RunSlotWork, &work));
        CHECK(work.initial == nullptr);
        CHECK(work.setSucceeded);
        CHECK(AtomicLoad(&record.calls) == 1);
        CHECK(record.callbackThread == work.thread);
        CHECK(record.localAfterWrite == CallbackMarker);
        CHECK(record.clearedInside);
    }
}

// 유효하지 않은 칸(정리 함수 없음)은 값을 받지 않는다.
TEST(ThreadExitSlot_InvalidSlotRejectsValues) {
    ThreadExitSlot invalid(nullptr);
    CHECK(!invalid.IsValid());
    int value = 0;
    CHECK(!invalid.Set(&value));
    CHECK(invalid.Get() == nullptr);
}
} // namespace thread_exit_slot_tests_file
