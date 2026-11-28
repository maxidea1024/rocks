#pragma once

#include <Base/Types.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialParameterMetadata.h>
#include <MaterialSchema/MaterialParameterType.h>
#include <MaterialSchema/MaterialReferenceIndex.h>
#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// 머티리얼 파라미터 표의 항목 하나다(Docs/Design/MaterialSystem 12.8절 MS3d2 — UE cached expression
// data의 파라미터 목록). 쿠킹 처리기가 노드 값 hook으로 채우고, 인스턴스 override 해석(MS6)과
// artifact 파라미터 slot이 이 표를 번호로 본다.
//
// 사용 예:
//   for (const MaterialParameterInfo& parameter : material.parameters) {
//       Show(parameter.name, parameter.defaults);
//   }
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialParameterInfo {
    ROCKS_GENERATED_BODY()

    explicit MaterialParameterInfo(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 0;
    ROCKS_PROPERTY() MaterialParameterType type = MaterialParameterType::Scalar;
    // custom primitive data 번호다(쓰지 않으면 NoPrimitiveDataIndex).
    ROCKS_PROPERTY() int32 primitiveDataIndex = NoPrimitiveDataIndex;
    // 수 기본값이다(성분 수만큼 — float도 double로).
    ROCKS_PROPERTY() double defaults[MaterialParameterComponents] = { 0.0, 0.0, 0.0, 0.0 };
    // 텍스처 파라미터의 기본 텍스처 Material::referencedTextures 번호다(없으면
    // NoMaterialReference).
    ROCKS_PROPERTY() uint32 texture = NoMaterialReference;
};

} // namespace rocks::material
