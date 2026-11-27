#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Material/MaterialFunctionBase.h>
#include <Material/TextureParameterOverride.h>
#include <MaterialSchema/AssetParameterOverride.h>
#include <MaterialSchema/DoubleVectorParameterOverride.h>
#include <MaterialSchema/FontParameterOverride.h>
#include <MaterialSchema/ScalarParameterOverride.h>
#include <MaterialSchema/StaticComponentMaskOverride.h>
#include <MaterialSchema/StaticEnumOverride.h>
#include <MaterialSchema/StaticSwitchOverride.h>
#include <MaterialSchema/VectorParameterOverride.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 머티리얼 함수 인스턴스 에셋이다(Docs/Design/MaterialSystem 4.2·4.6·4.6.1절, UE
// UMaterialFunctionInstance — MaterialFunctionInstance.h:26–74). 함수 안 파라미터의 기본값을
// 덮는다. 쿠킹 런타임에는 남지 않는다(Build 의존).
namespace rocks {

// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MATERIAL_API MaterialFunctionInstance final : public MaterialFunctionBase {
    ROCKS_GENERATED_BODY()

public:
#if ROCKS_WITH_EDITOR_DATA
    explicit MaterialFunctionInstance(Allocator& allocator) noexcept
        : scalarOverrides(allocator), vectorOverrides(allocator), doubleVectorOverrides(allocator),
          textureOverrides(allocator), collectionOverrides(allocator),
          textureCollectionOverrides(allocator), virtualTextureOverrides(allocator),
          sparseVolumeTextureOverrides(allocator), fontOverrides(allocator),
          staticSwitchOverrides(allocator), staticComponentMaskOverrides(allocator),
          staticEnumOverrides(allocator) {
    }
#else
    explicit MaterialFunctionInstance(Allocator&) noexcept {
    }
#endif

    // 모든 필드가 EditorOnly다: 함수 인스턴스는 쿠킹 데이터가 없고(4.2절) 쓰는 머티리얼에 펼쳐져
    // 들어가므로, 텍스처 등 참조가 쿠킹 패키지에 Hard 간선을 만들지 않게 한다(4.6.1절).
#if ROCKS_WITH_EDITOR_DATA
    // 부모 함수(함수 또는 함수 인스턴스)다. Build 의존이고 사슬 순환은 V3 오류다.
    ROCKS_PROPERTY(EditorOnly) ObjectRef<MaterialFunctionBase> parent;
    ROCKS_PROPERTY(EditorOnly) Array<material::ScalarParameterOverride> scalarOverrides;
    ROCKS_PROPERTY(EditorOnly) Array<material::VectorParameterOverride> vectorOverrides;
    ROCKS_PROPERTY(EditorOnly)
    Array<material::DoubleVectorParameterOverride> doubleVectorOverrides;
    ROCKS_PROPERTY(EditorOnly) StructRefArray<TextureParameterOverride> textureOverrides;
    ROCKS_PROPERTY(EditorOnly) Array<material::AssetParameterOverride> collectionOverrides;
    // TODO(MPQ-TextureCollection): 텍스처 컬렉션 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY(EditorOnly) Array<material::AssetParameterOverride> textureCollectionOverrides;
    // TODO(MPQ-VirtualTexture): RVT 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY(EditorOnly) Array<material::AssetParameterOverride> virtualTextureOverrides;
    // TODO(MPQ-SparseVolumeTexture): sparse volume texture 에셋이 생기면 대상 클래스를 검사한다.
    ROCKS_PROPERTY(EditorOnly)
    Array<material::AssetParameterOverride> sparseVolumeTextureOverrides;
    ROCKS_PROPERTY(EditorOnly) Array<material::FontParameterOverride> fontOverrides;
    ROCKS_PROPERTY(EditorOnly) Array<material::StaticSwitchOverride> staticSwitchOverrides;
    ROCKS_PROPERTY(EditorOnly)
    Array<material::StaticComponentMaskOverride> staticComponentMaskOverrides;
    ROCKS_PROPERTY(EditorOnly) Array<material::StaticEnumOverride> staticEnumOverrides;
#endif
};

} // namespace rocks
