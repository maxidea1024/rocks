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
ROCKS_PLATFORMBASE_API void WriteDiagnostic(const char* bytes, usize count) noexcept;

// Fatal·FatalAt이 크래시 수집기를 거쳐 끝낼 때의 프로세스 종료 코드다(Windows 예외 코드로도 쓴다).
// 사용자 정의 예외 코드 영역(0xE...)이라 OS NTSTATUS와 겹치지 않는다.
inline constexpr uint32 RocksFatalExceptionCode = 0xE0524B46;

// 처리기 안에서 다시 실패했을 때(같은 스레드 재귀)의 종료 코드다.
inline constexpr uint32 RocksRecursiveCrashExitCode = 0xE0524B52;

// 복구할 수 없는 invariant 실패에 사용한다. 유효한 문자열 또는 nullptr를 받는다.
// 1) stderr에 최대 4096 byte를 비상 출력한다(일반 logger·heap formatting 없음).
// 2) 크래시 수집기(Platform/CrashReporting.h)가 설치돼 있으면 공통 보고 경로(종료 콜백 — 마지막
//    로그 flush 등)를 거쳐 끝낸다. 설치돼 있지 않으면 바로 끝낸다. Windows는 디버거가 붙어
//    있으면(보고 강제 옵션이 없을 때) 이 프레임에서 멈춘 뒤 끝낸다(POSIX는 디버거가 신호를 먼저
//    받는다).
// Windows 종료 코드는 RocksFatalExceptionCode다. POSIX는 이전 SIGABRT 동작으로 abort한다. abort를
// SIGABRT 처리기로 되돌리는 순환은 없다. 부른 위치(return address)를 크래시 기록에 남긴다.
// 정상 종료의 자원 정리를 하지 않으며 출력의 완전한 저장도 보장하지 않는다.
[[noreturn]] ROCKS_PLATFORMBASE_API void Fatal(const char* message) noexcept;

// 위치를 아는 호출자(assert)용 Fatal이다. file은 정적 수명 문자열(nullptr 가능)이다.
[[noreturn]] ROCKS_PLATFORMBASE_API void FatalAt(const char* message, const char* file,
                                                 uint32 line) noexcept;

} // namespace rocks::platform
