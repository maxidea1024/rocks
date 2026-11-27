#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrModule.h>
#include <MaterialCompiler/MaterialTargetField.h>
#include <NodeGraph/GraphDiagnostic.h>

// 머티리얼 그래프 번역의 결과다(Docs/Design/MaterialSystem 6.8절). 오류 진단이 하나라도 있으면
// succeeded가 거짓이고 module은 비어 있다. 경고·정보 진단은 성공해도 남는다.
//
// 수명: allocator는 결과보다 오래 살아야 한다. 진단의 Name은 번역 입력의 NamePool 토큰이다.
namespace rocks::material {

struct MaterialTranslation {
    explicit MaterialTranslation(Allocator& allocator) noexcept
        : module(allocator), diagnostics(allocator), userSceneTextures(allocator),
          customOutputs(allocator) {
    }

    MaterialTranslation(const MaterialTranslation&) = delete;
    MaterialTranslation& operator=(const MaterialTranslation&) = delete;

    IrModule module;
    nodegraph::GraphDiagnosticList diagnostics;
    // 번역이 읽은 번역 대상 항목이다(6.9절 MS2b3 — 쿠킹이 번역 키에 넣는다).
    MaterialTargetFieldMask consultedTarget = NoMaterialTargetFields;
    // UserSceneTexture 노드가 읽는 이름이다(6.10절 MS2c4 — 처음 참조 순, 번호 k의 scene texture
    // id는 MaterialUserSceneTextureFirstId + k). pass가 이름으로 입력을 묶는다.
    Array<Name> userSceneTextures;
    // 내보낸 shading model 집합 비트다(Renderer 14.2절 번호 — 6.11절, 8.3절 MaterialShaderTraits).
    uint32 shadingModels = 0;
    // 결과가 있는 custom 출력 이름이다(6.11절 MS2d3 표 순서 — 8.3절 MaterialShaderTraits 거르기).
    // 정적 원문이다.
    Array<StringView> customOutputs;
    bool succeeded = false;
};

} // namespace rocks::material
