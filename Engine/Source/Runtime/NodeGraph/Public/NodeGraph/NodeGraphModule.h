#pragma once

#include <Core/Result.h>

// NodeGraph 모듈의 리플렉션 타입 등록이다(Docs/Design/NodeGraph.ko.md 6.1절).
namespace rocks {

class ModuleRegistration;

namespace nodegraph {

// 문서·배치 리플렉션 타입(GraphDocument.h)을 registration에 등록한다. 도메인 자산이 GraphDocument·
// GraphDocumentLayout 필드를 가지면 그 모듈의 등록보다 먼저 부른다.
//
// 오류: registration의 등록 오류를 그대로 돌려준다. 스레드: registration 소유자의 규칙을 따른다.
ROCKS_NODEGRAPH_API Result<void> RegisterNodeGraphTypes(ModuleRegistration& registration) noexcept;

} // namespace nodegraph

} // namespace rocks
