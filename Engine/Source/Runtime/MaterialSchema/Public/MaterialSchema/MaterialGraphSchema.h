#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/NamePool.h>
#include <MaterialSchema/MaterialGraphTypes.h>
#include <MaterialSchema/MaterialNodeDefinition.h>
#include <NodeGraph/GraphSchema.h>

// 머티리얼 그래프 도메인 schema다(Docs/Design/MaterialSystem 5절, 단계 MS1a). 그래프 종류마다
// 하나를 만든다: 머티리얼(도메인 "Material") 또는 머티리얼 함수(도메인 "MaterialFunction").
//
// - 핀 타입 표: MaterialPinTypes(5.3절).
// - 연결 판정: 수 군끼리, 같은 자원 타입끼리, TextureAny ← 텍스처 타입, wildcard(재경유 등)는 모두.
//   순환을 만들면 거부한다(V2). 폭·변환 오류는 번역의 V1이 낸다.
// - 기본 노드: 머티리얼 주 그래프에 출력 노드(Material.Output) 하나.
// - 의미 정규화: 정의의 nonSemanticFields(파라미터 기본값)를 속성 Record에서 뺀다(5.5절).
//
// 사용 예:
//   MaterialGraphSchema schema(MaterialGraphKind::Material, catalog, names, SystemAllocator());
//   if (!schema.Ready()) { ... }
//   registry.RegisterSchema(schema, lease);
//
// 스레드: 상태가 없어 여러 스레드에서 읽을 수 있다(GraphSchema 계약). 판정이 쓰는 임시 할당은 모두
// scratch로 한다. catalog 조회는 소유 lane에서 바뀌므로, 등록·해제와 판정을 같은 lane에서 한다.
// 수명: catalog·names·scratch는 schema보다 오래 살아야 한다. scratch는 의미 정규화가 잠깐 쓰는
// Record 사본과 기본 노드 값에 쓰며, 여러 스레드에서 판정하면 thread-safe allocator여야 한다.
namespace rocks::material {

class MaterialNodeCatalog;

class ROCKS_MATERIALSCHEMA_API MaterialGraphSchema final : public nodegraph::GraphSchema {
public:
    MaterialGraphSchema(MaterialGraphKind kind, const MaterialNodeCatalog& catalog, NamePool& names,
                        Allocator& scratch) noexcept;

    // 도메인·출력 노드 이름 intern에 성공했는가.
    bool Ready() const noexcept {
        return ready_;
    }

    MaterialGraphKind Kind() const noexcept {
        return kind_;
    }

    Name Domain() const noexcept override {
        return domain_;
    }

    const nodegraph::PinTypeTable& PinTypes() const noexcept override;

    Result<void>
    NormalizeSemanticNode(const nodegraph::GraphNode& node,
                          nodegraph::SemanticNodeWriter& writer) const noexcept override;

    nodegraph::ConnectVerdict
    CanConnect(const nodegraph::GraphReadView& view,
               const nodegraph::ConnectQuery& query) const noexcept override;

    Result<void> CreateDefaultNodes(nodegraph::GraphRole role, Guid graph,
                                    nodegraph::GraphIdSource& ids,
                                    nodegraph::GraphCommandList& out) const noexcept override;

private:
    MaterialGraphKind kind_;
    const MaterialNodeCatalog& catalog_;
    Allocator& scratch_;
    Name domain_;
    Name outputType_;
    bool ready_ = false;
};

} // namespace rocks::material
