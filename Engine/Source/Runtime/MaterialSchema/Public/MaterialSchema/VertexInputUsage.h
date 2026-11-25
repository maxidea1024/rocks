#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 머티리얼 사용 대상이다(Docs/Design/MaterialSystem 8.4·19.10절, 결정 MD6). 정점 입력 타입 이름의
// 집합이다(UE bUsedWith* 플래그 대응). 선언 값이고, 쿠커가 참조로 유도한 대상을 더한다(8.4절). 아직
// 없는 정점 입력 타입 이름도 담을 수 있다: 그 타입이 등록되기 전에는 쿠킹 진단(정보)이다.
namespace rocks::material {

// UE bUsedWithStaticMesh 기본값(true)에 대응하는 기본 정점 입력 타입 이름이다(MeshAnimationAssets
// MR4).
inline constexpr StringView StaticMeshVertexInputName = "StaticMesh";

ROCKS_STRUCT()
struct VertexInputUsage {
    ROCKS_GENERATED_BODY()

    explicit VertexInputUsage(Allocator& allocator) noexcept : vertexInputs(allocator) {
    }

    // 선언 순서이고 중복이 없다(AddVertexInput으로만 넣는다).
    ROCKS_PROPERTY() Array<Name> vertexInputs;
};

// name이 있는가(Name 비교 규칙).
ROCKS_MATERIALSCHEMA_API bool ContainsVertexInput(const VertexInputUsage& usage,
                                                  Name name) noexcept;

// name이 없으면 끝에 더한다(있으면 아무것도 하지 않고 성공). 오류: None 이름 InvalidArgument, 할당
// 실패.
ROCKS_MATERIALSCHEMA_API Result<void> AddVertexInput(VertexInputUsage& usage, Name name) noexcept;

} // namespace rocks::material
