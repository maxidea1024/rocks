#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/GraphReadView.h>
#include <NodeGraph/NodeTypeDescriptor.h>

// 도메인 그래프 schema다(Docs/Design/NodeGraph.ko.md 7·8.6절). schema는 정책만 맡는다(GD9). 지금
// 있는 것: 도메인 식별·핀 타입 표·의미 정규화(NG0), 연결 판정·기본 노드·검증(NG1). 붙여넣기 허용은
// NG2, 메뉴·drop은 NG5에서 더한다(14절).
namespace rocks::nodegraph {

class SemanticNodeWriter;
class GraphCommandList;
class GraphIdSource;
struct GraphDiagnosticList;

// 붙여넣기 판정이다(9.4절).
enum class PasteDecision : uint8 {
    Allow,
    Deny
};

// 연결 판정의 처리다(7절, UE ECanCreateConnectionResponse 대응).
enum class ConnectAction : uint8 {
    Deny,
    Make,
    // 입력 핀의 기존 간선을 빼고 잇는다.
    MakeReplacingInput,
    // 출력 핀의 기존 간선을 빼고 잇는다.
    MakeReplacingOutput,
    MakeReplacingBoth,
    // conversionNodeType 노드를 사이에 넣어 잇는다(첫 입력 핀·첫 출력 핀).
    MakeWithConversion
};

// 연결 판정 결과다.
struct ConnectVerdict {
    ConnectAction action = ConnectAction::Deny;
    // MakeWithConversion일 때의 변환 노드 종류다.
    Name conversionNodeType;
    // 거부·경고 사유(도메인 코드)다.
    uint32 reasonCode = 0;
};

// 연결 판정의 입력이다. 핀 서술은 판정 동안만 유효하다.
struct ConnectQuery {
    const Graph* graph = nullptr;
    PinRef from;
    PinRef to;
    const PinDescriptor* fromPin = nullptr;
    const PinDescriptor* toPin = nullptr;
    // 입력 핀·출력 핀에 지금 있는 간선 수다.
    uint32 toEdgeCount = 0;
    uint32 fromEdgeCount = 0;
};

// 핀 타입 하나의 정보다. PinTypeId는 표의 색인이다.
struct PinTypeInfo {
    // 영어 이름이다(진단·dump용).
    StringView name;
};

// 도메인의 핀 타입 표다.
struct PinTypeTable {
    Span<const PinTypeInfo> types;

    uint32 Count() const noexcept {
        return static_cast<uint32>(types.Size());
    }
};

// 도메인이 구현한다. 상태가 없어야 하고 여러 스레드에서 읽을 수 있어야 한다. 등록은
// NodeTypeRegistry::RegisterSchema다(한 도메인에 하나).
//
// 사용 예:
//   class MaterialSchema final : public GraphSchema {
//   public:
//       Name Domain() const noexcept override { return domain_; }
//       const PinTypeTable& PinTypes() const noexcept override { return types_; }
//       Result<void> NormalizeSemanticNode(const GraphNode& node,
//                                          SemanticNodeWriter& writer) const noexcept override {
//           // 파라미터 노드면 기본값을 뺀 속성 바이트로 쓴다(머티리얼 시스템 5.5절)
//           return writer.WriteNode(node);
//       }
//   };
class ROCKS_NODEGRAPH_API GraphSchema {
public:
    virtual ~GraphSchema() noexcept = default;

    // 도메인 식별이다. 등록 동안 바뀌지 않는다.
    virtual Name Domain() const noexcept = 0;

    // 핀 타입 표다. 등록 동안 바뀌지 않는다.
    virtual const PinTypeTable& PinTypes() const noexcept = 0;

    // 노드 하나의 의미 바이트를 writer에 쓴다(8.4절). writer.WriteNode를 정확히 한 번 부른다.
    // 기본은 노드 전체다. 도메인은 의미에서 빼야 할 필드(예: 머티리얼 파라미터 기본값)를 거른 속성
    // 바이트를 넘긴다. 결정적이어야 한다(같은 노드에 같은 바이트).
    virtual Result<void> NormalizeSemanticNode(const GraphNode& node,
                                               SemanticNodeWriter& writer) const noexcept;

    // 연결 판정이다. 동기·빠름(화면 hover마다 부른다). 기본: 핀 종류가 같고 타입이 같거나 한쪽이
    // UnresolvedPinType이면 연결하고, 입력 핀이 maxEdges만큼 차 있으면 MakeReplacingInput(UE 단일
    // 입력 핀 기본), 그 밖은 Deny다. 순환 금지가 필요한 도메인은
    // WouldCreateCycle(GraphReadView.h)로 본다.
    virtual ConnectVerdict CanConnect(const GraphReadView& view,
                                      const ConnectQuery& query) const noexcept;

    // 새 그래프(role)의 기본 노드 명령을 out에 덧붙인다(예: 머티리얼 출력 노드). id는 ids에서
    // 받는다. 기본은 아무것도 더하지 않는다.
    virtual Result<void> CreateDefaultNodes(GraphRole role, Guid graph, GraphIdSource& ids,
                                            GraphCommandList& out) const noexcept;

    // 도메인 구조 검사다(컴파일 진단과 별개, 빠름). 진단을 out에 더한다. 기본은 없음이다.
    virtual Result<void> Validate(const GraphReadView& view,
                                  GraphDiagnosticList& out) const noexcept;

    // 붙여넣기에서 노드 하나를 받을지다(9.4절 — 도메인 규칙, 예: 붙일 수 없는 출력 노드). 공통 규칙
    // (종류 버전·속성 schema·문서당 하나)은 붙여넣기가 먼저 본다. 기본은 Allow다.
    virtual PasteDecision CanPasteNode(const GraphReadView& view,
                                       const GraphNode& node) const noexcept;

    // 같은 문서 붙여넣기에서 바깥 간선(조각 노드와 밖 노드 사이)을 다시 이을지다. 기본은 거짓(끊기
    // — UE와 같다)이다.
    virtual bool AllowExternalEdgeReconnect() const noexcept;
};

} // namespace rocks::nodegraph
