#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialConstantInstruction.h>
#include <MaterialSchema/MaterialConstantSlot.h>
#include <MaterialSchema/MaterialTextureSlot.h>
#include <Meta/Annotations.h>

// 머티리얼 artifact 하나다(Docs/Design/MaterialSystem 12.4절, 12.8절 MS3d2 — UE FMaterialResource의
// 쿠킹 부분). 번역 결과 하나의 실행 데이터이고 순수 값이다(텍스처·컬렉션은 Material의 Hard 참조
// 필드 번호로, 파라미터는 Material::parameters 번호로 가리킨다). 셰이더 코드는 들지 않는다 —
// 셰이더 라이브러리에서 셰이더 집합 id로 연다(12.6절). 번호 필드는 리플렉션 해석이 대조하지
// 않으므로 로드가 ValidateMaterialArtifact로 한 번 검증한 뒤에 쓴다(손상은 CorruptData).
//
// 사용 예:
//   const MaterialArtifact& artifact = material.artifacts[entry.artifact];
//   const uint64 bundle = shader::MaterialShaderBundleId(artifact.shaderSetId);
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialArtifact {
    ROCKS_GENERATED_BODY()

    explicit MaterialArtifact(Allocator& allocator) noexcept
        : traits(allocator), constants(allocator), program(allocator), parameters(allocator),
          textures(allocator), collections(allocator), userSceneTextures(allocator) {
    }

    // 셰이더 집합 id다(8.6절 — 라이브러리 묶음 id는 앞 8 byte).
    ROCKS_PROPERTY() Sha256Digest shaderSetId {};
    // 상수 블록 layout 서명이다(인스턴스 패키지의 부모 layout 대조 — 12.4절).
    ROCKS_PROPERTY() Sha256Digest layoutSignature {};
    // MaterialShaderTraits 정규 바이트다(8.3절).
    ROCKS_PROPERTY() Array<uint8> traits;
    // 상수 블록 float4 수다(0이면 상수 블록이 없다).
    ROCKS_PROPERTY() uint32 floatVectors = 0;
    ROCKS_PROPERTY() Array<MaterialConstantSlot> constants;
    ROCKS_PROPERTY() Array<MaterialConstantInstruction> program;
    // artifact 파라미터 번호(Parameter 명령의 즉시값 [MaterialConstantTableImmediate] — 번역
    // 파라미터 표 순) → Material::parameters 번호다.
    ROCKS_PROPERTY() Array<uint32> parameters;
    // 텍스처 slot이다(MaterialTexture<k> 순).
    ROCKS_PROPERTY() Array<MaterialTextureSlot> textures;
    // 컬렉션 순(MaterialCollection<k>) → Material::parameterCollections 번호다.
    ROCKS_PROPERTY() Array<uint32> collections;
    // 사용자 장면 텍스처 이름이다(UE UserSceneTexture).
    ROCKS_PROPERTY() Array<Name> userSceneTextures;
};

} // namespace rocks::material
