#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Material/MaterialLightBakeSettings.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/MaterialTextureSamplingScale.h>

// 그리기에 쓸 수 있는 머티리얼 에셋의 공통 기반이다(Docs/Design/MaterialSystem 4.2·4.6·19.11절, UE
// UMaterialInterface). 머티리얼과 인스턴스가 함께 가지는 필드를 둔다.
namespace rocks {

// 물리 재질 표 칸 수다(UE PhysicalMaterialMap[8] — EPhysicalMaterialMaskColor 수).
inline constexpr usize MaterialPhysicalMaterialMapSize = 8;

// 메시 슬롯은 이 타입을 Hard로 참조한다(비면 엔진 기본 머티리얼).
//
// 사용 예(슬롯 필드):
//   ROCKS_PROPERTY() ObjectRef<MaterialBase> material;
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_MATERIAL_API MaterialBase : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit MaterialBase(Allocator& allocator) noexcept
        : subsurfaceProfiles(allocator), specularProfiles(allocator), toonProfiles(allocator),
          textureSamplingScales(allocator) {
    }

    // 번역이 노드 속성에서 모은 profile 목록이다(UE
    // SubsurfaceProfiles·SpecularProfiles·ToonProfiles).
    // TODO(MPQ-SubsurfaceProfile): subsurface profile 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY() Array<material::MaterialAssetReference> subsurfaceProfiles;
    // TODO(MPQ-SpecularProfile): specular profile 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY() Array<material::MaterialAssetReference> specularProfiles;
    // TODO(MPQ-ToonShading): toon profile 에셋과 Toon shading model이 생기면 쓴다.
    ROCKS_PROPERTY() Array<material::MaterialAssetReference> toonProfiles;
    // TODO(MPQ-NeuralProfile): neural profile 에셋이 생기면 신경망 후처리가 쓴다(UE NeuralProfile).
    ROCKS_PROPERTY() material::MaterialAssetReference neuralProfile;
    // 조명 굽기 설정이다(19.12절, UE LightmassSettings).
    ROCKS_PROPERTY() MaterialLightBakeSettings lightBakeSettings;
    // 텍스처 slot별 sampling scale이다(12.5절, UE TextureStreamingData). 번역이 채운다.
    ROCKS_PROPERTY() Array<material::MaterialTextureSamplingScale> textureSamplingScales;
};

} // namespace rocks
