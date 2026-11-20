#pragma once

#include <Core/Commandlet/Commandlet.h>

namespace rocks::commandlet_host {

// "content-sign" 명령이다(Docs/Design/ContentSigning.ko.md 5·7절 — A1f). 하위 명령:
//   content-sign keygen <out key blob> <out public> --password-file <path>
//   content-sign sign <in.rman> <out.rsig> --key <key blob> --password-file <path>
//                     --version <u64> --product <id> --channel <name>
//   content-sign verify <in.rsig> --trust <public> --product <id> --channel <name>
//                       [--minimum-version <u64>]
// 서명 키 파일(비밀번호 KeyBlob)은 저장소·쿠킹 결과에 넣지 않는다(도움말에도 적는다). 비밀번호는
// 파일로만 받는다. 사용법 오류 2, 파일·키·서명·검증 오류 1, 성공 0이다. context는 작업 메모리에
// 쓰는 Allocator*다(commandlet_host::Run이 건다).
Result<int32> RunContentSignCommand(void* context, Span<const StringView> args,
                                    CommandletOutput& output) noexcept;

} // namespace rocks::commandlet_host
