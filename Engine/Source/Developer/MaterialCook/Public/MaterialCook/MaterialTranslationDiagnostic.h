#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <NodeGraph/GraphDiagnostic.h>

// 번역 너겟 값에 담는 진단 하나다(Docs/Design/MaterialSystem 12.8절 MS3c "값" part 10 — 적중 때도
// 같은 경고를 재생한다). 이름(pin)은 NamePool 번호가 아니라 문자열이다. 루트 그래프 진단의 asset은
// null로 담고 재생할 때 쿠킹 대상 id를 붙인다.
namespace rocks {

struct MaterialTranslationDiagnostic {
    explicit MaterialTranslationDiagnostic(Allocator& allocator) noexcept
        : pin(allocator), message(allocator) {
    }

    Guid asset;
    Guid graph;
    Guid node;
    String pin;
    nodegraph::GraphDiagnosticSeverity severity = nodegraph::GraphDiagnosticSeverity::Warning;
    uint32 code = 0;
    String message;
};

} // namespace rocks
