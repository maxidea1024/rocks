#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// 진단 byte를 stderr로 바로 쓴다(Windows WriteFile, POSIX write(2)). CRT FILE 계층과 그 lock을
// 거치지 않으므로 다른 스레드가 CRT stderr lock을 쥐고 있어도 막히지 않는다(OS I/O 대기는 남는다).
// 힙 할당· 서식화가 없다. 로그 시스템의 비상 출력(설치 전, sink 콜백 안의 로그, Fatal)이 쓴다.
// 실패(닫힌 stderr 등)는 무시한다. 부분 쓰기는 남은 byte를 이어 쓰되, 진행이 없으면 멈춘다.
//
// 사용 예:
//   const char line[] = "[Warning][Log] queue saturated";
//   WriteDiagnostic(line, sizeof(line) - 1);
void WriteDiagnostic(const char* bytes, usize count) noexcept;

// 비정상 종료 직전에 한 번 부르는 hook이다(마지막 로그 flush, Docs/Design/LoggingSystem.ko.md L24).
// message는 종료 사유(NUL 종료, bytes byte)다. 막히는 lock을 잡지 말고 제한 시간 안에 돌아와야
// 한다. PlatformBase는 Core 아래라 StringView 대신 포인터와 길이를 받는다.
using TerminationHook = void (*)(const char* message, usize bytes) noexcept;

// 종료 hook을 정하고 앞 hook을 돌려준다(원자 교환). nullptr은 해제다. Fatal과 OS 예외·신호
// 처리기(CrashHandler.h)가 프로세스에서 한 번만 부른다.
TerminationHook SetTerminationHook(TerminationHook hook) noexcept;

// 종료 hook을 부르게 한 스레드(platform::Fatal을 부른 스레드, 크래시한 스레드)의
// id다(CurrentThreadId 값). hook 안에서 읽는다. 아직 없으면 0이다. 로그 flush는 이 스레드가 출력
// 스레드 자신이면 기다리지 않는다.
uint64 TerminatingThreadId() noexcept;

// 복구할 수 없는 invariant 실패에 사용한다. 유효한 문자열 또는 nullptr를 받으며
// 최대 4096 byte의 stderr 출력을 시도하고, 종료 hook(있으면, 한 번)을 부른 뒤 abort한다. 일반
// logger나 heap formatting은 없다. 정상 종료의 자원 정리를 수행하지 않으며 출력의 완전한 저장도
// 보장하지 않는다.
[[noreturn]] void Fatal(const char* message) noexcept;

} // namespace rocks::platform
