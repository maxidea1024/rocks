#pragma once

#include <Platform/Sync.h>

namespace rocks::platform {

enum class ThreadStatus {
    Success,
    InvalidArgument,
    AlreadyJoinable,
    NotJoinable,
    SelfJoin,
    Busy,
    SystemError
};

// OS 스케줄러에 주는 상대 우선순위다(UE EThreadPriority의 일반 단계 대응). 실시간·time-critical
// 단계는 두지 않는다. OS별 대응:
// - Windows: THREAD_PRIORITY_LOWEST·BELOW_NORMAL·NORMAL·ABOVE_NORMAL·HIGHEST
// - macOS: SCHED_OTHER 범위(최소·기본·최대)를 다섯 단계로 나눈 값
// - Android: nice 19·10·0·-4·-8(android.os.Process THREAD_PRIORITY_LOWEST·BACKGROUND·DEFAULT·
//   DISPLAY·URGENT_DISPLAY와 같은 값)
enum class ThreadPriority : uint8 {
    Lowest,
    BelowNormal,
    Normal,
    AboveNormal,
    Highest
};

// 스레드 이름의 최대 길이(UTF-8 byte, NUL 제외)다. 넘으면 Start가 InvalidArgument다.
inline constexpr usize MaxThreadNameBytes = 63;

// 스택 크기 상한(1 GiB)이다. 넘으면 Start가 InvalidArgument다.
inline constexpr usize MaxThreadStackBytes = usize{1} << 30;

// Thread::Start의 생성 옵션이다. 기본값은 이름 없음·보통 우선순위·플랫폼 기본 스택이다.
//
// 사용 예:
//   ThreadOptions options;
//   options.name = "RenderLane";
//   options.priority = ThreadPriority::AboveNormal;
//   options.stackBytes = 1024 * 1024;
//   thread.Start(Entry, &context, options);
//
// - name: NUL 종료 UTF-8이다. nullptr·빈 문자열이면 이름을 붙이지 않는다. Start가 복사하므로 호출이
//   끝나면 버려도 된다. 잘못된 UTF-8·MaxThreadNameBytes 초과는 InvalidArgument다.
//   디버거·프로파일러·크래시 보고에 보이는 진단용 이름이며, OS 한도가 더 짧으면(Android 15 byte)
//   문자 경계에서 자른다.
// - priority: 새 스레드가 entry를 부르기 전에 그 스레드에서 적용한다. 이름과 우선순위는 OS 힌트다.
//   OS가 거부해도(권한 부족 등) Start는 성공하고 스레드는 OS 기본값으로 실행한다.
// - stackBytes: 0이면 플랫폼 기본이다. 아니면 플랫폼 최소값·page 단위로 올려 예약한다(Windows는
//   예약 크기, POSIX는 pthread 스택 크기). MaxThreadStackBytes 초과는 InvalidArgument다. 예약할 수
//   없으면 SystemError이며 스레드를 만들지 않는다.
struct ThreadOptions {
    const char* name = nullptr;
    ThreadPriority priority = ThreadPriority::Normal;
    usize stackBytes = 0;
};

// native thread 하나의 join 소유권이다. entry/context는 Start 전에 준비하고 Join 성공까지 유지한다.
// 실행 함수는 noexcept이며 OS 종료 API로 wrapper를 건너뛰거나 thread를 강제 취소하지 않는다.
// Start 실패는 새 join 소유권을 만들지 않는다. Wait timeout/Join 실패는 소유권을 유지한다.
// Join 성공 뒤 재시작 가능하다. detach는 없고 joinable 상태 소멸은 Fatal로 수명 오류를 드러낸다.
// 동시 제어 요청은 gate로 중재한다. 진행 중 Join은 다른 Join/Start에 Busy/AlreadyJoinable을
// 반환한다. self Join은 SelfJoin이다. 객체 파괴는 모든 제어 호출이 끝난 뒤 수행한다.
class Thread {
public:
    using Entry = void (*)(void*) noexcept;

    Thread() noexcept;
    ~Thread() noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    // options는 위 ThreadOptions 계약을 따른다. 옵션 검사 실패는 새 스레드를 만들지 않는다.
    ThreadStatus Start(Entry entry, void* context = nullptr,
                       const ThreadOptions& options = {}) noexcept;
    // entry 함수의 반환까지 기다린다. TLS destructor/native thread 종료까지를 뜻하지 않는다.
    // timeout은 소유권을 바꾸지 않는다. 비joinable/self wait는 Failed다.
    WaitResult WaitForCompletion(uint32 timeoutMs = InfiniteTimeout) noexcept;
    // TLS 정리와 native 종료까지 회수한다. 제한시간 없는 대기이며 성공 후 context를 해제한다.
    ThreadStatus Join() noexcept;
    bool IsJoinable() const noexcept;

private:
    alignas(16) unsigned char storage_[320]{};
};

// SleepFor의 모든 값은 유한 millisecond다. 0은 즉시 반환한다. 실제 wake 시점의 정확성은 보장하지
// 않는다.
void SleepFor(uint32 milliseconds) noexcept;

// 현재 스레드의 OS 이름을 buffer에 UTF-8로 복사하고(NUL 포함) 길이(NUL 제외)를 돌려준다. 이름이
// 없거나 조회할 수 없거나 capacity가 모자라면 0이다(buffer는 capacity가 0이 아니면 빈 문자열).
// MaxThreadNameBytes + 1 byte buffer면 Thread::Start로 붙인 이름을 모두 받는다. 진단·시험용이다.
usize CopyCurrentThreadName(char* buffer, usize capacity) noexcept;
void YieldThread() noexcept;

} // namespace rocks::platform
