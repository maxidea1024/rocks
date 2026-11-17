#pragma once

#include <Config/ConfigContext.h>
#include <Core/Commandlet/Commandlet.h>
#include <Core/Text/Json.h>

// config-dump: 설정 문맥의 값과 출처를 결정적 JSON으로 쓴다(Docs/Design/ConfigSystem.ko.md 8절 —
// golden 파일과 비교하는 시험·도구가 쓴다).
//
// 형식(Core canonical JSON — 들여쓰기 2칸, LF, 주석 없음):
//   {
//     "sections": [                         // (branch, section) 이름의 byte 순서
//       {
//         "branch": "Render",
//         "section": "RenderGraph",
//         "values": { ... },                 // 리플렉션 ExportJson(Edit 용도 — 스키마 필드 순서)
//         "sources": [                       // 필드 경로의 byte 순서
//           { "path": "maxPasses",
//             "stack": [ { "layer": "Engine", "tag": "Render.json", "source": "..." }, ... ] }
//         ]
//       }
//     ],
//     "settings": [                         // 독립 콘솔 설정(이름 byte 순서)
//       { "name": "stream.pool_mb", "value": "512", "stack": [ ... ] }
//     ]
//   }
// stack은 약한 것부터이고 마지막이 현재 값의 출처다. 기본값뿐인 필드는 sources에 없다. 설정 struct
// 필드의 콘솔 이름은 그 섹션의 values·sources에 있고 settings에는 독립 설정만 둔다.
namespace rocks::config {

// 값과 출처를 writer에 객체 하나로 쓴다(Finish는 호출자가 한다). 마지막 Sync의 상태를 쓴다(큐는
// 보지 않는다). 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread, ExportJson·writer의 오류.
// 실패하면 writer는 실패 상태다(결과를 버린다).
//
// 사용 예:
//   StringBuilder<> text(allocator);
//   JsonWriter writer(text);
//   if (auto dumped = WriteConfigDump(context, writer); !dumped || !writer.Finish()) { ... }
Result<void> WriteConfigDump(ConfigContext& context, JsonWriter& writer) noexcept;

// config-dump Commandlet 이름이다(Commandlet 이름 문법 — 소문자·숫자·'-').
inline constexpr StringView ConfigDumpCommandletName = "config-dump";

// registry에 config-dump를 등록한다. 실행하면 WriteConfigDump 결과를 Output 스트림에 쓰고 0을
// 돌려준다(인자를 받지 않는다 — 인자가 있으면 Error 스트림에 사용법을 쓰고 2). context는 registry의
// 마지막 실행보다 오래 살아야 하고, 실행은 context의 소유 스레드(= registry의 소유 스레드)에서
// 한다. 오류: CommandletRegistry::Register의 오류(이름 중복 AlreadyExists 등).
Result<void> RegisterConfigDumpCommandlet(CommandletRegistry& registry,
                                          ConfigContext& context) noexcept;

} // namespace rocks::config
