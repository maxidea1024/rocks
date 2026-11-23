#pragma once

#include <Core/Commandlet/Commandlet.h>
#include <NodeGraph/NodeTypeRegistry.h>
#include <Object/TypeRegistry.h>

// 노드 그래프 commandlet이다(Docs/Design/NodeGraph 9.4절 "commandlet"): graph-dump·
// graph-validate·graph-diff. 입력은 문서 형식(`rocks.graph-document`) 텍스트 파일이다. 명령마다
// Name pool·리플렉션 TypeRegistry·노드 종류 등록표를 새로 만들고 문맥의 도메인 등록 hook으로
// 채운다.
//
// 종료 코드: 0 성공(같음·오류 진단 없음), 1 차이·오류 진단 있음, 2 사용법 오류, 3 입력
// 오류(파일·형식· 크기), 4 내부 오류.
//
// 사용 예(호스트):
//   GraphCommandletContext graphContext;
//   graphContext.allocator = &allocator;
//   auto registered = RegisterGraphCommandlets(registry, graphContext);
namespace rocks::nodegraph {

inline constexpr int32 GraphCommandSuccess = 0;
inline constexpr int32 GraphCommandDifferent = 1;
inline constexpr int32 GraphCommandUsage = 2;
inline constexpr int32 GraphCommandInputError = 3;
// 할당 실패·등록 hook 실패·문맥 오류 같은 내부 오류다(오류 줄 "<명령>: internal error (<오류
// 이름>)").
inline constexpr int32 GraphCommandInternalError = 4;

// graph-validate가 등록되지 않은 도메인에 내는 진단 코드다(공통 불변식 GI 번호와 겹치지 않는 값).
inline constexpr uint32 GraphDomainNotRegisteredCode = 1000;

// 도메인 등록 hook이다. setupNodes는 명령마다 새로 만든 노드 종류 등록표에 도메인 schema·노드
// 종류를 등록하고(문맥이 등록표를 주면 부르지 않는다), setupTypes는 명령마다 새로 만든 리플렉션
// TypeRegistry에 속성·값 struct를 등록한다. 실패하면 그 명령이 오류로 끝난다.
using GraphNodeSetup = Result<void> (*)(void* user, NodeTypeRegistry& nodes) noexcept;
using GraphTypeSetup = Result<void> (*)(void* user, TypeRegistry& types) noexcept;

// commandlet 문맥이다. names·registry는 호스트가 이미 가진 Name pool과 노드 종류 등록표다(편집기
// 프로세스처럼 — 둘 다 주거나 둘 다 비운다). 비우면 명령마다 새로 만들고 setupNodes로 채운다.
// 등록된 도메인이 없으면 모든 노드가 자리 노드로 읽히고 graph-validate는 도메인 미등록을 보고한다.
// 문맥과 그것이 가리키는 것은 등록한 CommandletRegistry보다 오래 살아야 한다.
struct GraphCommandletContext {
    Allocator* allocator = nullptr;
    NamePool* names = nullptr;
    NodeTypeRegistry* registry = nullptr;
    GraphNodeSetup setupNodes = nullptr;
    GraphTypeSetup setupTypes = nullptr;
    void* user = nullptr;
};

// graph-dump <파일> [--semantic-only] [--records] [--out=<파일>]: 읽고 정규화해 다시 쓴다(--out이
// 없으면 표준 출력).
ROCKS_NODEGRAPHTOOLS_API Result<int32>
RunGraphDumpCommand(void* context, Span<const StringView> args, CommandletOutput& output) noexcept;

// graph-validate <파일>: 불변식 검사(GI1–GI14)와 도메인 schema Validate의 진단을 쓴다.
ROCKS_NODEGRAPHTOOLS_API Result<int32> RunGraphValidateCommand(void* context,
                                                               Span<const StringView> args,
                                                               CommandletOutput& output) noexcept;

// graph-diff <파일 A> <파일 B> [--semantic-only]: 차이를 한 줄씩 쓴다.
ROCKS_NODEGRAPHTOOLS_API Result<int32>
RunGraphDiffCommand(void* context, Span<const StringView> args, CommandletOutput& output) noexcept;

// 세 명령을 registry에 등록한다.
ROCKS_NODEGRAPHTOOLS_API Result<void>
RegisterGraphCommandlets(CommandletRegistry& registry, GraphCommandletContext& context) noexcept;

} // namespace rocks::nodegraph
