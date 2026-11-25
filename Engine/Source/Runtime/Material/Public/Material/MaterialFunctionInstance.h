#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Material/MaterialFunction.h>
#include <Material/TextureParameterOverride.h>
#include <MaterialSchema/DoubleVectorParameterOverride.h>
#include <MaterialSchema/ScalarParameterOverride.h>
#include <MaterialSchema/StaticComponentMaskOverride.h>
#include <MaterialSchema/StaticSwitchOverride.h>
#include <MaterialSchema/VectorParameterOverride.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 머티리얼 함수 인스턴스 에셋이다(Docs/Design/MaterialSystem 4.2·4.6절, UE
// UMaterialFunctionInstance). 함수 안 파라미터의 기본값을 덮는다. 쿠킹 런타임에는 남지 않는다(Build
// 의존).
namespace rocks {

// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MATERIAL_API MaterialFunctionInstance final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit MaterialFunctionInstance(Allocator& allocator) noexcept
        : scalarOverrides(allocator), vectorOverrides(allocator), doubleVectorOverrides(allocator),
          textureOverrides(allocator), staticSwitchOverrides(allocator),
          staticComponentMaskOverrides(allocator) {
    }

    // 부모 함수(Build)다. 사슬 순환은 V3 오류다.
    ROCKS_PROPERTY() ObjectRef<MaterialFunction> parent;
    ROCKS_PROPERTY() Array<material::ScalarParameterOverride> scalarOverrides;
    ROCKS_PROPERTY() Array<material::VectorParameterOverride> vectorOverrides;
    ROCKS_PROPERTY() Array<material::DoubleVectorParameterOverride> doubleVectorOverrides;
    ROCKS_PROPERTY() StructRefArray<TextureParameterOverride> textureOverrides;
    ROCKS_PROPERTY() Array<material::StaticSwitchOverride> staticSwitchOverrides;
    ROCKS_PROPERTY() Array<material::StaticComponentMaskOverride> staticComponentMaskOverrides;
};

} // namespace rocks
