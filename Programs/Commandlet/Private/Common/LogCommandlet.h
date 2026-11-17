#pragma once

#include <Core/Commandlet/Commandlet.h>

namespace rocks::commandlet_host {

// "log" 명령이다(Docs/Design/LoggingSystem.ko.md 9.1절, LG10). 하위 명령:
//   log keygen <out-public> <out-private>
//   log decode <file> --key <private key file> [--json]
//   log seal <plain input> <out-file> --public <public key file>
// 개인키는 저장소·쿠킹 결과에 넣지 않는다(도움말에도 적는다). 사용법 오류 2, 파일·복호 오류 1,
// 성공 0이다.
Result<int32> RunLogCommand(void* context, Span<const StringView> args,
                            CommandletOutput& output) noexcept;

} // namespace rocks::commandlet_host
