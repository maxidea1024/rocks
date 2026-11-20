#pragma once

#include <Core/Commandlet/Commandlet.h>

namespace rocks::commandlet_host {

// 명령을 등록하고 실행한다. 인수와 출력 채널은 진입점(Main.cpp)이 소유하며 이 함수는 실행 중에만
// 빌린다. 도구 로그(Sync LogSystem·stderr sink)는 Launch가 설치한 상태로 부른다.
int Run(Span<const StringView> args, CommandletOutput output) noexcept;

// 플랫폼 표준 출력 채널이다(Private/<Platform>/StandardOutput.cpp). Windows는 콘솔이면 UTF-8 글자
// 경계 조각을 WriteConsoleW로, 리디렉션이면 UTF-8 byte를 그대로 쓴다. macOS는 stdio에 쓰고 비운다.
CommandletOutput StandardOutput() noexcept;

// 본문 시작에서 한 번 부른다. macOS는 SIGPIPE를 무시해 닫힌 pipe 쓰기를 오류로 받는다. 실패하면
// false다(종료 코드 1).
bool PrepareStandardOutput() noexcept;

} // namespace rocks::commandlet_host
