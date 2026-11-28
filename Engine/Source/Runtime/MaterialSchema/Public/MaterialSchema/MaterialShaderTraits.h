#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialContextFeature.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/MaterialQualityLevel.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <RenderData/MaterialEnums.h>

// 머티리얼 셰이더 특성이다(Docs/Design/MaterialSystem 8.3절 — UE FMaterialShaderParameters 대응).
// 번역 결과에서 만들고(MaterialCompiler MakeMaterialShaderTraits), pass 셰이더 타입·정점 입력
// 타입의 거르기와 셰이더 집합 id(8.6절)가 읽는다. 쿠커(Developer)와 게임(Runtime)이 함께 쓰므로
// 장치 없는 MaterialSchema에 둔다.
//
// 사용 예:
//   MaterialShaderTraits traits(allocator);
//   ... MakeMaterialShaderTraits(translation, desc, quality, traits) ...
//   Array<uint8> bytes(allocator);
//   if (auto written = WriteMaterialShaderTraits(traits, bytes); !written) { ... }
namespace rocks::material {

// 정규 바이트 형식 버전이다(배치가 바뀌면 올린다 — 셰이더 집합 id가 바뀐다).
inline constexpr uint32 MaterialShaderTraitsVersion = 1;

struct MaterialShaderTraits {
    explicit MaterialShaderTraits(Allocator& allocator) noexcept : customOutputs(allocator) {
    }

    MaterialDomain domain = MaterialDomain::Surface;
    BlendMode blendMode = BlendMode::Opaque;
    bool twoSided = false;
    // 쓰는 shading model 집합 비트다(Renderer 14.2절 번호).
    uint32 shadingModels = 0;
    // 반투명 블렌드 모드일 때만 의미가 있다(아니면 기본값).
    TranslucencyPass translucencyPass = TranslucencyPass::AfterDepthOfField;
    bool writesVelocity = false;
    bool castsShadow = true;
    bool usesWpo = false;
    bool usesPreviousWpo = false;
    bool usesPdo = false;
    bool usesOpacityMask = false;
    // 정점·픽셀 문맥이 쓰는 문맥 기능 집합과 최소 기능 등급이다(6.4절 capability, 19.3절).
    MaterialContextFeatureMask vertexFeatures = NoMaterialContextFeatures;
    MaterialContextFeatureMask pixelFeatures = NoMaterialContextFeatures;
    MaterialCapabilityTier minimumTier = MaterialCapabilityTier::Mobile;
    // 결과가 있는 custom 출력 이름이다(6.11절 표 순서).
    Array<String> customOutputs;
    // 엔진 기본 머티리얼이다(늘 컴파일 대상 — UE bUsedAsSpecialEngineMaterial).
    bool engineDefaultMaterial = false;
    MaterialQualityLevel quality = MaterialQualityLevel::None;
};

// traits의 정규 바이트를 out 끝에 덧붙인다(8.3절 배치 — little-endian). 오류: 이름이 u32 길이를
// 넘음 CapacityExceeded, 할당 실패.
ROCKS_MATERIALSCHEMA_API Result<void> WriteMaterialShaderTraits(const MaterialShaderTraits& traits,
                                                                Array<uint8>& out) noexcept;

// 정규 바이트를 해석해 out을 채운다(out.customOutputs는 비우고 채운다). consumed는 읽은 byte
// 수다. 오류: 버전·열거 값·길이가 맞지 않음 CorruptData, 할당 실패.
ROCKS_MATERIALSCHEMA_API Result<void> ReadMaterialShaderTraits(Span<const uint8> bytes,
                                                               MaterialShaderTraits& out,
                                                               usize& consumed) noexcept;

} // namespace rocks::material
