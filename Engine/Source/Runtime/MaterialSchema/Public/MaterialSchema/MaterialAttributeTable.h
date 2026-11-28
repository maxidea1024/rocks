#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <MaterialSchema/MaterialAttributeDefinition.h>

// 머티리얼 속성 표다(Docs/Design/MaterialSystem 19.12절, UE FMaterialAttributeDefinitionMap과
// AddCustomAttribute). 내장 속성(BuiltinMaterialAttributes)과 플러그인·프로젝트의 custom 속성을
// 같은 표에 둔다.
//
// 사용 예:
//   MaterialAttributeTable attributes(allocator);
//   auto builtins = attributes.Register(BuiltinMaterialAttributes(), lease);
//   const MaterialAttributeDefinition* roughness = attributes.Find(RoughnessAttributeId);
//
// 스레드: 소유 lane 하나 전용이다(동기화 없음). 수명: allocator는 표보다 오래 살아야 한다. Find가
// 돌려준 포인터는 그 정의를 등록한 묶음이 해제될 때까지 유효하다.
namespace rocks::material {

// 등록 묶음 하나의 식별이다. 0은 없음이다.
struct MaterialAttributeGroupId {
    uint64 value = 0;
};

class ROCKS_MATERIALSCHEMA_API MaterialAttributeTable {
public:
    explicit MaterialAttributeTable(Allocator& allocator) noexcept;
    ~MaterialAttributeTable() noexcept = default;

    MaterialAttributeTable(const MaterialAttributeTable&) = delete;
    MaterialAttributeTable& operator=(const MaterialAttributeTable&) = delete;

    // 정의 묶음을 등록한다. 검사(InvalidArgument): id 0, 이름 비어 있음, 타입 범위 밖, 기본값 성분
    // 수가 MaxMaterialPinDefaultComponents 초과, frequency가 정점·픽셀 하나가 아님, 묶음 안 id
    // 중복. 이미 있는 id면 AlreadyExists다. 실패하면 표는 그대로다.
    Result<MaterialAttributeGroupId> Register(Span<const MaterialAttributeDefinition> definitions,
                                              ModuleLease lease) noexcept;

    // 묶음을 해제한다. 없는 id면 NotFound다.
    Result<void> Unregister(MaterialAttributeGroupId id) noexcept;

    // id인 정의다. 없으면 nullptr이다.
    const MaterialAttributeDefinition* Find(Guid id) const noexcept;

    usize Size() const noexcept {
        return entries_.Size();
    }

    // index번째 등록 정의다(등록 순 — 번역 키가 id 순으로 다시 고른다, 12.8절). index < Size().
    const MaterialAttributeDefinition& At(usize index) const noexcept {
        return *entries_[index].definition;
    }

private:
    struct Entry {
        const MaterialAttributeDefinition* definition = nullptr;
        MaterialAttributeGroupId group;
    };

    struct Group {
        MaterialAttributeGroupId id;
        ModuleLease lease;
    };

    Array<Entry> entries_;
    Array<Group> groups_;
    uint64 nextGroup_ = 1;
};

// 내장 속성이다(UE MaterialAttributeDefinitionMap.cpp:395–434 — 같은 Guid·타입·기본값·빈도).
// deprecated 두 값(WorldDisplacement·TessellationMultiplier)과 UE 내부 표지(Missing)는 넣지 않는다.
// 수명: 정적이다.
ROCKS_MATERIALSCHEMA_API Span<const MaterialAttributeDefinition>
BuiltinMaterialAttributes() noexcept;

} // namespace rocks::material
