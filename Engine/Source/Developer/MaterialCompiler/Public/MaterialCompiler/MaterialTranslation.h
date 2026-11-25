#pragma once

#include <Core/Memory/Allocator.h>
#include <MaterialCompiler/MaterialIrModule.h>
#include <NodeGraph/GraphDiagnostic.h>

// 머티리얼 그래프 번역의 결과다(Docs/Design/MaterialSystem 6.8절). 오류 진단이 하나라도 있으면
// succeeded가 거짓이고 module은 비어 있다. 경고·정보 진단은 성공해도 남는다.
//
// 수명: allocator는 결과보다 오래 살아야 한다. 진단의 Name은 번역 입력의 NamePool 토큰이다.
namespace rocks::material {

struct MaterialTranslation {
    explicit MaterialTranslation(Allocator& allocator) noexcept
        : module(allocator), diagnostics(allocator) {
    }

    MaterialTranslation(const MaterialTranslation&) = delete;
    MaterialTranslation& operator=(const MaterialTranslation&) = delete;

    IrModule module;
    nodegraph::GraphDiagnosticList diagnostics;
    bool succeeded = false;
};

} // namespace rocks::material
