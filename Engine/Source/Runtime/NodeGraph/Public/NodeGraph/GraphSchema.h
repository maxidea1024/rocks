#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <NodeGraph/GraphDocument.h>

// 도메인 그래프 schema다(Docs/Design/NodeGraph.ko.md 7절). schema는 정책만 맡는다(GD9). NG0은
// 도메인 식별·핀 타입 표·의미 정규화만 둔다. 나머지(연결 판정·검증·붙여넣기·기본 노드·메뉴·drop)는
// 쓰는 단계에서 더한다(14절).
namespace rocks::nodegraph {

class SemanticNodeWriter;

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
};

} // namespace rocks::nodegraph
