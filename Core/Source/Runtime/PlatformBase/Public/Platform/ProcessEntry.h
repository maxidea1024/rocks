#pragma once

#include <Platform/File.h>

// 프로세스 진입점과 프로세스 종료 요청의 OS 경계다. 설계는
// Docs/Design/CrashHandlingAndLaunch.ko.md 7.3절·7.4절·20절이다. 공통 실행 계층(Launch)과 Core
// ProcessCommandLine이 쓴다. 실행 파일이 직접 부를 일은 없다.
namespace rocks::platform {

// 진입점이 받은 인자 한 칸을 UTF-8로 buffer(capacity byte, NUL 포함)에 쓰고 쓴 길이(NUL 제외)를
// bytes에 둔다. 할당하지 않는다. 진입점 형태는 플랫폼마다 하나다(D7).
// - wchar_t 판: Windows wmain·wWinMain 인자(UTF-16)를 바꾼다(Platform/Win64/WideString.h와
//   같은 규칙, 대체 문자로 바꾸지 않는다). 짝 없는 surrogate는 InvalidArgument
//   (ERROR_NO_UNICODE_TRANSLATION)다. 다른 플랫폼은 Unsupported다.
// - char 판: POSIX main 인자를 그대로 복사한다. UTF-8 검증은 호출자가 한다(인자 byte는 OS가
//   준 그대로다). Windows는 Unsupported다(좁은 문자 argv는 ANSI 변환으로 비 ASCII 인자가
//   깨진다).
// 공통 실패: argument가 nullptr이거나 buffer가 nullptr이면 InvalidArgument, 칸이 모자라면
// Overflow이고 bytes에 필요한 길이(NUL 제외, 셀 수 없으면 0)를 둔다. 실패하면 capacity가 1 이상일
// 때 buffer는 빈 문자열이다.
//
// 사용 예(ProcessCommandLine::Capture):
//   usize bytes = 0;
//   if (const FileStatus status = ConvertEntryArgument(argv[index], cursor, remaining, bytes);
//   !status) {
//       return status;
//   }
ROCKS_PLATFORMBASE_API FileStatus ConvertEntryArgument(const wchar_t* argument, char* buffer,
                                                       usize capacity, usize& bytes) noexcept;
ROCKS_PLATFORMBASE_API FileStatus ConvertEntryArgument(const char* argument, char* buffer,
                                                       usize capacity, usize& bytes) noexcept;

// Windows GUI 진입점(wWinMain)의 인자다. UCRT가 명령줄을 나눠 둔 __argc·__wargv를 돌려준다(첫 칸은
// 실행 파일). wWinMain 실행 파일에서 진입 뒤 언제든 같은 값이다. 다른 플랫폼이거나 CRT가 넓은 문자
// 인자를 채우지 않았으면(좁은 문자 진입점) false이고 출력은 바꾸지 않는다.
//
// 사용 예(Launch RunWindowedApplication):
//   int argc = 0;
//   wchar_t** argv = nullptr;
//   if (!GetWindowedEntryArguments(argc, argv)) { ... 실패 ... }
ROCKS_PLATFORMBASE_API bool GetWindowedEntryArguments(int& argc, wchar_t**& argv) noexcept;

// 프로세스 종료 요청(Windows 콘솔 Ctrl-C·Ctrl-Break, POSIX SIGINT·SIGTERM)을 받을 함수다. OS 처리
// 문맥(Windows 콘솔 제어 스레드, POSIX 신호 처리기)에서 불리므로 원자 변수 쓰기만
// 한다(async-signal-safe).
using TerminationRequestHandler = void (*)() noexcept;

// 종료 요청 처리기를 건다. 첫 요청은 handler를 부르고 프로세스를 끝내지 않는다. 두 번째 요청은
// handler를 부르지 않고 OS 기본 동작(즉시 종료)으로 넘긴다(Windows는 기본 콘솔 처리기가
// STATUS_CONTROL_C_EXIT로 끝내고, POSIX는 설치 전 동작을 되돌리고 같은 신호를 다시 낸다). 콘솔 창
// 닫기·로그오프·시스템 종료는 받지 않는다(OS 기본 동작). 프로세스에 하나만 건다. 이미 걸려 있거나
// handler가 nullptr이거나 OS 등록이 실패하면 false이고 아무것도 바꾸지 않는다. 시작 단계(다른
// 스레드가 생기기 전)에 부른다.
//
// 사용 예(Launch):
//   if (!InstallTerminationRequestHandler(&OnTerminationRequest)) { ... 경고 ... }
//   ...
//   RemoveTerminationRequestHandler();
ROCKS_PLATFORMBASE_API bool
InstallTerminationRequestHandler(TerminationRequestHandler handler) noexcept;

// 처리기를 떼고 OS 처리를 설치 전으로 되돌린다. 걸려 있지 않으면 아무 일도 하지 않는다. 받은 요청
// 수도 지운다. 종료 단계에서 부른다.
ROCKS_PLATFORMBASE_API void RemoveTerminationRequestHandler() noexcept;

} // namespace rocks::platform
