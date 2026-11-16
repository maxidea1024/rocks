#include <TestSupport/Test.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

struct NameCapture {
    char name[MaxThreadNameBytes + 1]{};
    usize length = 0;
};

void CaptureName(void* raw) noexcept {
    auto& capture = *static_cast<NameCapture*>(raw);
    capture.length = CopyCurrentThreadName(capture.name, sizeof(capture.name));
}

void JoinOrFatal(Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != WaitResult::Signaled ||
        thread.Join() != ThreadStatus::Success) {
        Fatal("Thread option fixture failed to terminate");
    }
}

// 기본 스택(Windows 1 MiB)보다 큰 지역 배열을 page마다 건드린다. stackBytes가 적용되지 않으면 stack
// overflow로 프로세스가 끝나므로 시험이 실패로 드러난다.
void TouchLargeStack(void* raw) noexcept {
    constexpr usize Bytes = 3 * 512 * 1024;
    volatile uint8 buffer[Bytes];
    for (usize offset = 0; offset < Bytes; offset += 4096) {
        buffer[offset] = static_cast<uint8>(offset >> 12);
    }

    usize sum = 0;
    for (usize offset = 0; offset < Bytes; offset += 4096) {
        sum += buffer[offset];
    }

    *static_cast<usize*>(raw) = sum;
}

void MarkRan(void* raw) noexcept {
    *static_cast<bool*>(raw) = true;
}

} // namespace

TEST(Thread_OptionsNameIsVisibleInsideThread) {
    ThreadOptions options;
    options.name = "RocksOptionTest";
    NameCapture capture;
    Thread thread;
    REQUIRE(thread.Start(CaptureName, &capture, options) == ThreadStatus::Success);
    JoinOrFatal(thread);
    // Linux 계열의 15 byte 한도에 맞춘 이름이라 모든 플랫폼에서 그대로 보여야 한다.
    CHECK(capture.length == 15);
    CHECK(MemoryEquals(capture.name, "RocksOptionTest", 16));

    // 여러 byte 문자 이름은 플랫폼 한도에서 문자 경계로 잘릴 수 있으므로 앞부분 일치만 본다.
    const char korean[] = "렌더Lane스레드";
    options.name = korean;
    NameCapture wide;
    REQUIRE(thread.Start(CaptureName, &wide, options) == ThreadStatus::Success);
    JoinOrFatal(thread);
    CHECK(wide.length >= 6);
    CHECK(wide.length < sizeof(korean));
    CHECK(MemoryEquals(wide.name, korean, wide.length));

    // 이름이 없는 스레드에 앞 실행의 이름이 남지 않는다(같은 Thread 객체 재사용).
    NameCapture unnamed;
    REQUIRE(thread.Start(CaptureName, &unnamed) == ThreadStatus::Success);
    JoinOrFatal(thread);
    CHECK(unnamed.length == 0 || !MemoryEquals(unnamed.name, korean, 6));

    char small[4]{};
    CHECK(CopyCurrentThreadName(small, 0) == 0);
    CHECK(CopyCurrentThreadName(nullptr, 8) == 0);
}

TEST(Thread_OptionsRejectInvalidInputWithoutStarting) {
    Thread thread;
    bool ran = false;

    char longName[MaxThreadNameBytes + 2]{};
    for (usize index = 0; index < MaxThreadNameBytes + 1; ++index) {
        longName[index] = 'x';
    }

    ThreadOptions options;
    options.name = longName;
    CHECK(thread.Start(MarkRan, &ran, options) == ThreadStatus::InvalidArgument);
    CHECK(!thread.IsJoinable());

    // 정확히 한도 길이는 받는다.
    longName[MaxThreadNameBytes] = '\0';
    CHECK(thread.Start(MarkRan, &ran, options) == ThreadStatus::Success);
    JoinOrFatal(thread);
    CHECK(ran);

    // 과대 표현(C0 80)·대리 영역(ED A0 80)·잘린 다중 byte 문자는 잘못된 UTF-8이다.
    const char overlong[] = {static_cast<char>(0xC0), static_cast<char>(0x80), '\0'};
    const char surrogate[] = {static_cast<char>(0xED), static_cast<char>(0xA0),
                              static_cast<char>(0x80), '\0'};
    const char truncated[] = {'a', static_cast<char>(0xE3), static_cast<char>(0x81), '\0'};
    const char* invalidNames[] = {overlong, surrogate, truncated};
    for (const char* name : invalidNames) {
        options.name = name;
        CHECK(thread.Start(MarkRan, &ran, options) == ThreadStatus::InvalidArgument);
        CHECK(!thread.IsJoinable());
    }

    ThreadOptions stack;
    stack.stackBytes = MaxThreadStackBytes + 1;
    CHECK(thread.Start(MarkRan, &ran, stack) == ThreadStatus::InvalidArgument);
    ThreadOptions priority;
    priority.priority = static_cast<ThreadPriority>(9);
    CHECK(thread.Start(MarkRan, &ran, priority) == ThreadStatus::InvalidArgument);
    CHECK(!thread.IsJoinable());
}

TEST(Thread_OptionsStackAndPriorityApply) {
    Thread thread;
    ThreadOptions options;
    options.stackBytes = 8 * 1024 * 1024;
    usize sum = 0;
    REQUIRE(thread.Start(TouchLargeStack, &sum, options) == ThreadStatus::Success);
    JoinOrFatal(thread);
    CHECK(sum != 0);

    // 우선순위는 OS 힌트라 적용 결과를 공통으로 읽을 수 없다. 모든 단계에서 시작·실행·Join되는지
    // 본다.
    const ThreadPriority priorities[] = {ThreadPriority::Lowest, ThreadPriority::BelowNormal,
                                         ThreadPriority::Normal, ThreadPriority::AboveNormal,
                                         ThreadPriority::Highest};
    for (ThreadPriority priority : priorities) {
        ThreadOptions prioritized;
        prioritized.priority = priority;
        prioritized.stackBytes = 256 * 1024;
        bool ran = false;
        REQUIRE(thread.Start(MarkRan, &ran, prioritized) == ThreadStatus::Success);
        JoinOrFatal(thread);
        CHECK(ran);
    }
}
