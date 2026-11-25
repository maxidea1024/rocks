#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialNodeDefinition.h>
#include <MaterialSchema/MaterialNodeGroupId.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 머티리얼 노드 정의의 등록과 조회다(Docs/Design/MaterialSystem 5.2·19.7절).
//
// 정의 묶음을 도메인(머티리얼·머티리얼 함수) NodeTypeRegistry에 등록하고, 종류 이름 → 정의 조회
// 표를 둔다. 엔진 노드(BuiltinMaterialNodeGroups)와 플러그인 노드가 같은 함수로 등록한다. 등록은
// 정의를 검사하고 NodeGraph 서술(핀 키·종류 이름 intern, 속성 struct ID)을 만든다. 정의의
// graphKinds에 그 그래프 종류가 없으면 그 도메인에는 등록하지 않는다.
//
// 사용 예:
//   MaterialNodeCatalog catalog(allocator);
//   auto group = catalog.Register(registry, materialSchema, BuiltinMaterialNodes(), lease);
//   const MaterialNodeDefinition* add = catalog.Find(addTypeName);
//   catalog.Unregister(registry, group.Value());
//
// 스레드: NodeTypeRegistry와 같은 소유 lane 전용이다(동기화 없음).
// 수명: allocator는 catalog보다 오래 살아야 한다. 정의 data는 lease가 지킨다. Find가 돌려준
// 포인터는 그 정의를 등록한 묶음이 해제될 때까지 유효하다.
namespace rocks::material {

class MaterialGraphSchema;

class ROCKS_MATERIALSCHEMA_API MaterialNodeCatalog {
public:
    explicit MaterialNodeCatalog(Allocator& allocator) noexcept;
    ~MaterialNodeCatalog() noexcept = default;

    MaterialNodeCatalog(const MaterialNodeCatalog&) = delete;
    MaterialNodeCatalog& operator=(const MaterialNodeCatalog&) = delete;

    // definitions를 schema의 도메인에 등록한다. schema는 registry에 이미 등록되어 있어야 한다.
    //
    // 정의 검사(InvalidArgument): 종류 이름 비어 있음, 핀 키 비어 있음·중복(ASCII 대소문자 무시 —
    // NamePool 비교 규칙), 통과 쌍 키가 정적 입력·출력 핀이 아님, 핀 타입 범위 밖,
    // 출력 핀의 기본값·입력 표지, TextureAny 출력, 정의되지 않은 표지 비트, 기본값 성분 수 4 초과,
    // 상태 Implemented인데 선행 있음·PrerequisiteMissing인데 선행 None, nonSemanticFields가 있는데
    // propertiesSchema 없음·필드 이름이 스키마에 없음, stages 0·정의 밖 비트, graphKinds 정의 밖
    // 비트, 묶음 안 종류 이름 중복(대소문자 무시). 이미 다른 정의로 등록된 종류 이름은
    // AlreadyExists다. 그 밖은 NodeTypeRegistry::RegisterNodeType의 오류(같은 종류가 이미 있으면
    // AlreadyExists 등)와 할당 오류다. 실패하면 이 묶음의 등록은 모두 되돌린다.
    Result<MaterialNodeGroupId> Register(nodegraph::NodeTypeRegistry& registry,
                                         const MaterialGraphSchema& schema,
                                         Span<const MaterialNodeDefinition> definitions,
                                         ModuleLease lease) noexcept;

    // 여러 정의 배열을 묶음 하나로 등록한다(엔진 노드 — BuiltinMaterialNodeGroups).
    // 검사·오류·되돌림은 Register와 같고, 종류 이름 중복은 배열 사이에서도 본다.
    Result<MaterialNodeGroupId>
    RegisterGroups(nodegraph::NodeTypeRegistry& registry, const MaterialGraphSchema& schema,
                   Span<const Span<const MaterialNodeDefinition>> groups,
                   ModuleLease lease) noexcept;

    // 묶음을 해제한다(등록의 역순). 없는 id면 NotFound다. registry에서 이미 빠진 종류(NotFound)는
    // 빠진 것으로 본다. 그 밖의 해제 오류(통지 콜백 안의 Busy 등)가 나면 그 오류를 돌려주고 남은
    // 종류는 등록된 채로 둔다(다시 부를 수 있다).
    Result<void> Unregister(nodegraph::NodeTypeRegistry& registry, MaterialNodeGroupId id) noexcept;

    // 종류 이름의 정의다. 같은 종류 이름은 모든 도메인에서 같은 정의여야 한다(다른 정의로 등록하면
    // AlreadyExists) — 그래서 도메인 없이 찾는다. 없으면 nullptr이다.
    const MaterialNodeDefinition* Find(Name type) const noexcept;

    // 도메인 등록 수다(같은 정의를 두 도메인에 등록하면 둘이다).
    usize RegistrationCount() const noexcept {
        return entries_.Size();
    }

private:
    struct Entry {
        Name type;
        const MaterialNodeDefinition* definition = nullptr;
        MaterialNodeGroupId group;
        // 이 종류의 도메인 등록 id다(도메인당 하나).
        nodegraph::NodeTypeRegistrationId registration;
    };

    void RebuildLookup() noexcept;

    Allocator& allocator_;
    Array<Entry> entries_;
    // 살아 있는 묶음 id다(정의가 모두 걸러져 등록이 0개인 묶음도 있다).
    Array<uint64> groups_;
    // 종류 이름 → 정의다. 다시 만들다 할당에 실패하면 lookupComplete_가 거짓이고 Find가 선형으로
    // 찾는다(정확성은 표와 무관하다).
    HashMap<Name, const MaterialNodeDefinition*> lookup_;
    bool lookupComplete_ = true;
    uint64 nextGroup_ = 1;
};

} // namespace rocks::material
