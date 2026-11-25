#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/ScalarCollectionParameter.h>
#include <MaterialSchema/VectorCollectionParameter.h>

// 머티리얼 파라미터 컬렉션 에셋이다(Docs/Design/MaterialSystem 4.2·9.7절, UE
// UMaterialParameterCollection). 쿠킹 데이터는 layout과 기본값이다(World 상태가 쓴다).
namespace rocks {

// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MATERIAL_API ParameterCollection final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit ParameterCollection(Allocator& allocator) noexcept
        : scalars(allocator), vectors(allocator) {
    }

    ROCKS_PROPERTY() Array<material::ScalarCollectionParameter> scalars;
    ROCKS_PROPERTY() Array<material::VectorCollectionParameter> vectors;
};

} // namespace rocks
