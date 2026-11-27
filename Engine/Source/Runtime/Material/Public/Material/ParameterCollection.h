#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/ScalarCollectionBaseOverride.h>
#include <MaterialSchema/ScalarCollectionParameter.h>
#include <MaterialSchema/VectorCollectionBaseOverride.h>
#include <MaterialSchema/VectorCollectionParameter.h>
#include <Object/ObjectRefs.h>

// 머티리얼 파라미터 컬렉션 에셋이다(Docs/Design/MaterialSystem 4.2·4.6.1·9.7절, UE
// UMaterialParameterCollection). 쿠킹 데이터는 layout과 기본값이다(World 상태가 쓴다).
namespace rocks {

// UE 5.8처럼 다른 컬렉션을 상속할 수 있다(MaterialParameterCollection.h:217–228): 부모의 항목과
// layout을 물려받고 기본값만 덮는다. 상속 사슬 순환은 오류다.
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MATERIAL_API ParameterCollection final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit ParameterCollection(Allocator& allocator) noexcept
        : scalars(allocator), vectors(allocator), scalarBaseOverrides(allocator),
          vectorBaseOverrides(allocator) {
    }

    ROCKS_PROPERTY() Array<material::ScalarCollectionParameter> scalars;
    ROCKS_PROPERTY() Array<material::VectorCollectionParameter> vectors;
    // 부모 컬렉션(Hard — 실행 때 layout·기본값이 부모에서 온다)이다.
    ROCKS_PROPERTY() ObjectRef<ParameterCollection> base;
    ROCKS_PROPERTY() Array<material::ScalarCollectionBaseOverride> scalarBaseOverrides;
    ROCKS_PROPERTY() Array<material::VectorCollectionBaseOverride> vectorBaseOverrides;
};

} // namespace rocks
