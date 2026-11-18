#pragma once

#include <Base/Types.h>

// 비정상 종료 때 마지막 로그를 남기기 위한 최소 처리기다(Docs/Design/LoggingSystem.ko.md 14.1.8,
// L24·L25). crash 수집(minidump·스택)은 하지 않는다. 처리기가 하는 일은 종료 hook(Fatal.h
// SetTerminationHook) 한 번 부르기뿐이고, 처리 결과는 앞 처리기로 넘긴다(디버거·이후 crash
// reporter와 겹치지 않게 연쇄한다).
//
// - Windows: unhandled exception filter. stack overflow 때 filter는 깨진 스레드의 남은 stack에서
//   돌기 때문에 hook을 그 스레드에서 부르지 않는다. 설치 때 만든 flush 전용 스레드에 커널 이벤트로
//   넘기고, 완료 이벤트를 제한 시간만큼 기다린다(사용자 공간 lock 없음). platform::Thread로 만든
//   스레드는 시작 때 SetThreadStackGuarantee로 처리기용 여유 stack을 확보한다.
// - POSIX: SIGSEGV·SIGBUS·SIGILL·SIGFPE·SIGABRT 처리기(SA_ONSTACK). 미리 만든 flush 스레드를
//   sem_post(async-signal-safe)로 깨우고 완료 표시를 단조 시계로 제한 시간까지 확인한 뒤, 앞 동작을
//   되돌리고 신호를 다시 낸다. 신호 처리기 쪽 flush는 최선 노력이다(hook 경로는 async-signal-safe
//   보장 밖이다). platform::Thread로 만든 스레드와 설치한 스레드는 sigaltstack을 가진다.
//
// 비용: 기록마다 드는 비용은 없다. 프로세스당 대기만 하는 스레드 하나(작은 stack)와 커널 이벤트 둘
// (Windows) 또는 세마포어 하나(POSIX), 스레드마다 여유 stack 32 KiB(Windows 보장분·POSIX
// sigaltstack, 할당 없음). 크래시 때 최대 지연은 flushTimeoutMilliseconds다.
//
// 사용 예(엔진 호스트 main):
//   platform::InstallCrashHandler({.flushTimeoutMilliseconds = 2000});
//   ... LogInstallScope가 종료 hook을 건다 ...
//   platform::RemoveCrashHandler();   // 다른 스레드가 끝난 종료 단계에서
namespace rocks::platform {

// 처리기용 여유 stack 크기다(Windows SetThreadStackGuarantee, POSIX sigaltstack).
inline constexpr usize CrashStackReserveBytes = 32 * 1024;

struct CrashHandlerOptions {
    static constexpr uint32 DefaultFlushTimeoutMilliseconds = 2000;

    // 종료 hook(마지막 로그 flush)을 기다리는 상한이다.
    uint32 flushTimeoutMilliseconds = DefaultFlushTimeoutMilliseconds;
};

// 처리기를 설치한다(프로세스에 하나, 시작 단계에서). 이미 설치했으면 true를 돌려주고 아무것도 하지
// 않는다. flush 스레드를 만들지 못하면 false다(설치하지 않는다).
bool InstallCrashHandler(const CrashHandlerOptions& options = {}) noexcept;

// 처리기를 떼고 앞 처리기를 되돌린 뒤 flush 스레드를 끝낸다. 다른 스레드가 끝난 종료 단계에서만
// 부른다.
void RemoveCrashHandler() noexcept;

// 호출 스레드가 처리기를 돌릴 여유 stack을 확보한다. platform::Thread가 시작 때 부르고,
// InstallCrashHandler가 설치한 스레드에 부른다. 그 밖의 스레드(외부 라이브러리)는 직접 부른다.
void PrepareThreadForCrashHandling() noexcept;

} // namespace rocks::platform
