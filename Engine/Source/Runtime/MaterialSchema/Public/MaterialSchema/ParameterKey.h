#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>

// 머티리얼 파라미터 식별이다(Docs/Design/MaterialSystem 9.2절). 이름이 아니라 (정의 에셋,
// ParameterId, scope)로 식별해, 이름 변경·레이어 순서 변경·함수 반복 호출에도 override가 키로
// 남는다(MS-T9).
namespace rocks::material {

// scope 종류다(9.2절).
ROCKS_ENUM()
enum class ParameterScopeKind : uint8 {
    Global = 0,
    FunctionCall = 1,
    Layer = 2,
    LayerBlend = 3
};

ROCKS_STRUCT()
struct ParameterScope {
    ROCKS_GENERATED_BODY()

    explicit ParameterScope(Allocator& allocator) noexcept : callPath(allocator) {
    }

    ROCKS_PROPERTY() ParameterScopeKind kind = ParameterScopeKind::Global;
    // Layer·LayerBlend의 레이어 Guid다(그 밖은 0).
    ROCKS_PROPERTY() Guid layer;
    // 함수 호출 위치 NodeId 경로(바깥 → 안)다.
    ROCKS_PROPERTY() Array<Guid> callPath;
};

ROCKS_STRUCT()
struct ParameterKey {
    ROCKS_GENERATED_BODY()

    explicit ParameterKey(Allocator& allocator) noexcept : scope(allocator) {
    }

    // 파라미터 노드가 있는 에셋(머티리얼 또는 함수)이다.
    ROCKS_PROPERTY() AssetId ownerAssetId;
    // 파라미터 노드의 안정 Guid다(이름과 무관).
    ROCKS_PROPERTY() Guid parameterId;
    ROCKS_PROPERTY() ParameterScope scope;
};

} // namespace rocks::material
