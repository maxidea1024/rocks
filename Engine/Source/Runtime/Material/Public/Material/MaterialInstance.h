#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Optional.h>
#include <Material/MaterialBase.h>
#include <Material/MaterialLayerStack.h>
#include <Material/MaterialLightBakeOverrides.h>
#include <Material/TextureParameterOverride.h>
#include <Material/UserSceneTextureOverride.h>
#include <MaterialSchema/AssetParameterOverride.h>
#include <MaterialSchema/DoubleVectorParameterOverride.h>
#include <MaterialSchema/DynamicBoolParameterOverride.h>
#include <MaterialSchema/FontParameterOverride.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/MaterialTraitOverrides.h>
#include <MaterialSchema/ScalarParameterOverride.h>
#include <MaterialSchema/StaticComponentMaskOverride.h>
#include <MaterialSchema/StaticEnumOverride.h>
#include <MaterialSchema/StaticSwitchOverride.h>
#include <MaterialSchema/VectorParameterOverride.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 머티리얼 인스턴스 에셋이다(Docs/Design/MaterialSystem 4.2·4.6·4.6.1·19.11절, UE
// UMaterialInstanceConstant).
namespace rocks {

// 부모 머티리얼의 파라미터 값과 기본 속성을 덮는다. 값 override는 9.2절 키로 저장한다. static
// 값·기본 속성·레이어 스택을 덮으면 자기 static 변형을 쿠킹한다(4.2절). Optional override는 값이
// 없으면 부모 값을 쓰고, null 참조 값이면 "없음으로 덮기"다(4.6.1절).
//
// 사용 예:
//   material::ScalarParameterOverride roughness(allocator);
//   roughness.key = key;
//   roughness.value = 0.4f;
//   instance->scalarOverrides.TryPushBack(Move(roughness));
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MATERIAL_API MaterialInstance final : public MaterialBase {
    ROCKS_GENERATED_BODY()

public:
    explicit MaterialInstance(Allocator& allocator) noexcept
        : MaterialBase(allocator), scalarOverrides(allocator), vectorOverrides(allocator),
          doubleVectorOverrides(allocator), textureOverrides(allocator),
          dynamicBoolOverrides(allocator), collectionOverrides(allocator),
          textureCollectionOverrides(allocator), virtualTextureOverrides(allocator),
          sparseVolumeTextureOverrides(allocator), fontOverrides(allocator),
          userSceneTextureOverrides(allocator), staticSwitchOverrides(allocator),
          staticComponentMaskOverrides(allocator), staticEnumOverrides(allocator),
          traitOverrides(allocator)
#if ROCKS_WITH_EDITOR_DATA
          ,
          layerStack(allocator)
#endif
    {
    }

    // 부모(Hard + Build — 4.4절)다. 인스턴스 사슬 순환은 V3 오류다.
    ROCKS_PROPERTY() ObjectRef<MaterialBase> parent;

    ROCKS_PROPERTY() Array<material::ScalarParameterOverride> scalarOverrides;
    ROCKS_PROPERTY() Array<material::VectorParameterOverride> vectorOverrides;
    ROCKS_PROPERTY() Array<material::DoubleVectorParameterOverride> doubleVectorOverrides;
    ROCKS_PROPERTY() StructRefArray<TextureParameterOverride> textureOverrides;
    ROCKS_PROPERTY() Array<material::DynamicBoolParameterOverride> dynamicBoolOverrides;
    // 파라미터 컬렉션 교체다(UE ParameterCollectionParameterValues).
    ROCKS_PROPERTY() Array<material::AssetParameterOverride> collectionOverrides;
    // TODO(MPQ-TextureCollection): 텍스처 컬렉션 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY() Array<material::AssetParameterOverride> textureCollectionOverrides;
    // TODO(MPQ-VirtualTexture): RVT 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY() Array<material::AssetParameterOverride> virtualTextureOverrides;
    // TODO(MPQ-SparseVolumeTexture): sparse volume texture 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY() Array<material::AssetParameterOverride> sparseVolumeTextureOverrides;
    ROCKS_PROPERTY() Array<material::FontParameterOverride> fontOverrides;
    ROCKS_PROPERTY() Array<UserSceneTextureOverride> userSceneTextureOverrides;

    // static 값(static 변형 — 9.5절)이다.
    ROCKS_PROPERTY() Array<material::StaticSwitchOverride> staticSwitchOverrides;
    ROCKS_PROPERTY() Array<material::StaticComponentMaskOverride> staticComponentMaskOverrides;
    ROCKS_PROPERTY() Array<material::StaticEnumOverride> staticEnumOverrides;
    ROCKS_PROPERTY() material::MaterialTraitOverrides traitOverrides;
    ROCKS_PROPERTY() MaterialLightBakeOverrides lightBakeOverrides;

#if ROCKS_WITH_EDITOR_DATA
    // 레이어 스택을 덮는가와 그 값이다(static 변형 번역의 입력 — 4.6.1절). 함수 참조는 Build
    // 의존이다.
    ROCKS_PROPERTY(EditorOnly) bool overridesLayerStack = false;
    ROCKS_PROPERTY(EditorOnly) MaterialLayerStack layerStack;
#endif

    // TODO(MPQ-PhysicalMaterial): 물리 재질 에셋이 생기면 대상 클래스를 검사한다.
    // physicalMaterialOverride는 값이 없으면 부모 값, null 값이면 엔진 기본 물리 재질이다(UE
    // bOverridePhysMaterial·PhysMaterial). mask와 map은 UE MIC처럼 인스턴스 자기 값만 쓰고 부모로
    // 내려가지 않는다(UE UMaterialInstanceConstant::GetPhysicalMaterialMask·
    // UMaterialInstance::GetPhysicalMaterialFromMap — 4.6.1절).
    ROCKS_PROPERTY() Optional<material::MaterialAssetReference> physicalMaterialOverride;
    ROCKS_PROPERTY() material::MaterialAssetReference physicalMaterialMask;
    ROCKS_PROPERTY()
    material::MaterialAssetReference physicalMaterialMap[MaterialPhysicalMaterialMapSize];
    // TODO(MPQ-VirtualizedGeometry): 가상화 geometry 대체 머티리얼 override.
    ROCKS_PROPERTY() Optional<material::MaterialAssetReference> virtualizedGeometryOverride;
    // TODO(MPQ-SubsurfaceProfile): subsurface profile override.
    ROCKS_PROPERTY() Optional<material::MaterialAssetReference> subsurfaceProfileOverride;
    // TODO(MPQ-SpecularProfile): specular profile override.
    ROCKS_PROPERTY() Optional<material::MaterialAssetReference> specularProfileOverride;
    // TODO(MPQ-ToonShading): toon profile override.
    ROCKS_PROPERTY() Optional<material::MaterialAssetReference> toonProfileOverride;
};

} // namespace rocks
