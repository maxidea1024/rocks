#pragma once

#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <MaterialCompiler/MaterialConstantMember.h>
#include <MaterialCompiler/MaterialIrParameter.h>
#include <MaterialCompiler/MaterialIrTexture.h>
#include <MaterialCompiler/MaterialIrValue.h>
#include <MaterialCompiler/MaterialSourceLocation.h>
#include <MaterialCompiler/MaterialTargetField.h>
#include <MaterialCook/MaterialTranslationDiagnostic.h>
#include <MaterialSchema/MaterialShaderTraits.h>

// 번역 너겟 값이다(Docs/Design/MaterialSystem 12.2절 값 목록·12.8절 MS3c "값"). 번역·생성 결과에서
// 만들거나(MakeMaterialTranslationValue) 너겟 값에서 해석해(ReadMaterialTranslationNugget) 쿠킹
// 처리기가 쓴다. 기본값(파라미터 defaults)과 기본 텍스처 AssetId(파라미터 텍스처의 defaultAsset)는
// 담지 않는다 — 적중이면 처리기가 그래프에서 다시 모은다.
namespace rocks {

struct MaterialTranslationValue {
    explicit MaterialTranslationValue(Allocator& allocator) noexcept
        : source(allocator), members(allocator), steps(allocator), parameters(allocator),
          textures(allocator), traits(allocator), locations(allocator), diagnostics(allocator),
          userSceneTextures(allocator) {
    }

    // part 1 — 생성 원문(/Engine/Generated/Material.rsh 텍스트).
    String source;
    // part 2 — 상수 블록 layout(멤버·float4 수).
    Array<material::MaterialConstantMember> members;
    uint32 floatVectors = 0;
    // part 3 — 상수 식 프로그램과 파라미터 표(기본값 0 — 처리기가 그래프에서 채운다).
    Array<material::IrValue> steps;
    Array<material::IrParameter> parameters;
    // part 4 — 텍스처 slot(파라미터 slot의 defaultAsset은 null).
    Array<material::IrTexture> textures;
    // part 5 — 셰이더 특성.
    material::MaterialShaderTraits traits;
    // part 6 — 텍스처 sampling scale(12.5절)은 MS3d가 채운다(TODO(MS3d) — 지금 빈 목록).
    // part 7 — 원문 위치 지도.
    Array<material::MaterialSourceLocation> locations;
    // part 8 — 통계(IR 값 수).
    uint32 irValues = 0;
    // part 9 — IR hash.
    Sha256Digest irHash{};
    // part 10 — 진단 전체(경고·정보).
    Array<MaterialTranslationDiagnostic> diagnostics;
    // part 11 — UserSceneTexture 이름(처음 참조 순).
    Array<String> userSceneTextures;
    // part 12 — 번역이 읽은 번역 대상 항목.
    material::MaterialTargetFieldMask consultedTarget = material::NoMaterialTargetFields;
};

} // namespace rocks
