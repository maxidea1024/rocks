#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <NodeGraph/GraphDocument.h>

// 핀·노드 종류 서술이다(Docs/Design/NodeGraph 6.2절). 서술은 리플렉션 타입이 아니다 — 도메인
// 모듈이 코드로 만들어 NodeTypeRegistry에 등록한다.
//
// 서술의 StringView·Span이 가리키는 data는 등록한 모듈의 것이고, 등록 때 넘긴 module lease가 수명을
// 지킨다. 등록표는 핀·통과 쌍 배열을 소유 복사한다. 표시 문자열은 영어다(런타임 메시지 규칙).
//
// 사용 예(입력 둘·출력 하나인 곱셈 노드):
//   const PinDescriptor pins[] = {
//       InputPin(names.TryIntern("A").Value(), floatType, "A"),
//       InputPin(names.TryIntern("B").Value(), floatType, "B"),
//       OutputPin(names.TryIntern("Result").Value(), floatType, "Result"),
//   };
//   NodeTypeDescriptor multiply;
//   multiply.type = names.TryIntern("Material.Multiply").Value();
//   multiply.schemaVersion = 1;
//   multiply.staticPins = pins;
//   multiply.menu.displayName = "Multiply";
//   registry.RegisterNodeType(materialDomain, multiply, ModuleLease());
namespace rocks::nodegraph {

// wildcard 핀의 타입이다(도메인이 해석 — GD10).
inline constexpr PinTypeId UnresolvedPinType = NumericLimits<uint32>::Max();

// 간선 수 제한이 없는 핀의 maxEdges 표지다(출력 핀 기본).
inline constexpr uint16 UnlimitedEdges = NumericLimits<uint16>::Max();

// 노드 하나의 핀 수 상한(정적 + 동적)이다.
inline constexpr uint32 MaxPinsPerNode = 1024;

// 입력 기본값의 편집 정책이다.
enum class PinValuePolicy : uint8 {
    Editable = 0,
    ReadOnly = 1,
    Hidden = 2
};

// 핀 하나의 서술이다.
struct PinDescriptor {
    // 안정 키다. 표시 이름과 독립이다(GD2).
    Name key;
    PinDirection direction = PinDirection::Input;
    PinKind kind = PinKind::Data;
    // 도메인 타입 표 색인이다. wildcard면 UnresolvedPinType이다.
    PinTypeId type = UnresolvedPinType;
    // 입력은 기본 1이다. 출력은 OutputPin이 UnlimitedEdges로 만든다. 1 이상이다.
    uint16 maxEdges = 1;
    // 출력 핀만: 간선 순번이 의미 있다(GD4).
    bool ordered = false;
    // 고급 표시 묶음이다.
    bool advanced = false;
    PinValuePolicy valuePolicy = PinValuePolicy::Editable;
    // 영어 표시 이름이다. 가리키는 data는 핀 목록보다 오래 살아야 한다(서술 모듈 data 또는 노드
    // 속성 바이트).
    StringView displayName;
};

// 입력 핀 서술(간선 1개)을 만든다.
constexpr PinDescriptor InputPin(Name key, PinTypeId type, StringView displayName,
                                 PinKind kind = PinKind::Data) noexcept {
    PinDescriptor pin;
    pin.key = key;
    pin.direction = PinDirection::Input;
    pin.kind = kind;
    pin.type = type;
    pin.maxEdges = 1;
    pin.displayName = displayName;
    return pin;
}

// 출력 핀 서술(간선 무제한)을 만든다.
constexpr PinDescriptor OutputPin(Name key, PinTypeId type, StringView displayName,
                                  PinKind kind = PinKind::Data) noexcept {
    PinDescriptor pin;
    pin.key = key;
    pin.direction = PinDirection::Output;
    pin.kind = kind;
    pin.type = type;
    pin.maxEdges = UnlimitedEdges;
    pin.displayName = displayName;
    return pin;
}

// 비활성 노드에서 입력 간선이 이어지는 출력이다(6.5절).
struct PassThroughPair {
    Name input;
    Name output;
};

// 노드 종류의 capability 비트다.
enum class NodeCapabilities : uint32 {
    None = 0,
    CanDuplicate = 1u << 0,
    CanDelete = 1u << 1,
    // 문서에 하나만 둘 수 있다(예: 머티리얼 출력 노드).
    SinglePerDocument = 1u << 2,
    // NodeEnableState::Disabled를 쓸 수 있다(GI13).
    AllowDisable = 1u << 3
};

// 정의된 capability 비트 전체다(등록 검사가 그 밖의 비트를 거부한다).
inline constexpr uint32 NodeCapabilityMask = (1u << 4) - 1;

constexpr NodeCapabilities operator|(NodeCapabilities left, NodeCapabilities right) noexcept {
    return static_cast<NodeCapabilities>(static_cast<uint32>(left) | static_cast<uint32>(right));
}

constexpr bool HasCapability(NodeCapabilities set, NodeCapabilities flag) noexcept {
    return (static_cast<uint32>(set) & static_cast<uint32>(flag)) == static_cast<uint32>(flag);
}

// 노드 종류의 기본 capability다.
inline constexpr NodeCapabilities DefaultNodeCapabilities =
    NodeCapabilities::CanDuplicate | NodeCapabilities::CanDelete;

// 메뉴·palette 정보다(영어). 검사 한도는 화면 단계(NG5)에서 UiToolkit UD14·UD16 상수로 정한다.
struct NodeMenuInfo {
    StringView displayName;
    // 분류 경로다(예: "Math|Arithmetic").
    StringView category;
    StringView keywords;
    StringView tooltip;
};

// 노드 하나의 핀 목록이다(정적 핀 + 동적 핀). NodeTypeRegistry::CollectPins가 채운다.
//
// 수명: 핀의 displayName은 원래 data를 가리킨다(복사하지 않는다). allocator는 목록보다 오래 살아야
// 한다.
class ROCKS_NODEGRAPH_API PinList {
public:
    explicit PinList(Allocator& allocator) noexcept : pins_(allocator) {
    }

    // 핀을 끝에 더한다. key가 None이거나 이미 있으면 InvalidArgument, MaxPinsPerNode를 넘으면
    // CapacityExceeded, 할당 실패는 그 오류다. 실패하면 목록은 그대로다.
    Result<void> Add(const PinDescriptor& pin) noexcept;

    // key인 핀이다(Name 비교 규칙). 없으면 nullptr이다. 포인터는 다음 Add·Clear까지 유효하다.
    const PinDescriptor* Find(Name key) const noexcept;

    Span<const PinDescriptor> View() const noexcept {
        return pins_.View();
    }

    usize Size() const noexcept {
        return pins_.Size();
    }

    void Clear() noexcept {
        pins_.Clear();
    }

private:
    Array<PinDescriptor> pins_;
};

// 동적 핀 함수의 입력이다. 문서 읽기 view다(CollapsedGraph가 하위 그래프 경계에서 핀을 유도한다 —
// GD25). names는 동적 핀 키를 만들 때 쓰는 pool이다(문서 Name과 같은 pool).
struct DynamicPinsInput {
    const GraphDocument& document;
    const Graph& graph;
    const GraphNode& node;
    NamePool& names;
};

// 노드 속성(과 문서)에서 정적 핀 뒤에 붙을 동적 핀을 out에 더한다. 상태 없이 같은 입력에 같은
// 결과를 낸다. 속성을 해석할 수 없으면 InvalidArgument를 돌려준다.
using DynamicPinsFn = Result<void> (*)(const DynamicPinsInput& input, PinList& out) noexcept;

// migration 한 노드가 거칠 수 있는 단계 수 상한이다(10.1절 — 사슬이 끝나지 않는 함수를 막는다).
inline constexpr uint32 MaxMigrationSteps = 64;

// migration 한 단계의 입력이다(10.1절). node는 지금까지의 단계를 거친 노드다(종류·버전·속성과
// 입력 기본값·고아 핀 키가 앞 단계 결과를 반영한다). fromVersion은 node의 버전이다.
struct NodeMigrationInput {
    const GraphDocument& document;
    const Graph& graph;
    const GraphNode& node;
    uint32 fromVersion;
    NamePool& names;
};

// 핀 키 재지정 하나다(옛 키 → 새 키).
struct PinKeyRename {
    PinKey from;
    PinKey to;
};

// migration 한 단계의 결과다. 부르기 전에 노드의 종류·버전·속성 사본으로 채워 두고 renames는
// 비운다. 함수는 버전을 반드시 올린다: 같은 종류면 fromVersion < schemaVersion ≤ 서술 버전, 다른
// 종류(1:1 교체 — 같은 도메인, "Graph." 종류 아님)면 1 ≤ schemaVersion ≤ 그 종류의 서술 버전이다.
// properties는 결과 종류의 propertiesStruct Record여야 한다.
struct NodeMigrationResult {
    explicit NodeMigrationResult(Allocator& allocator) noexcept
        : properties(allocator), renames(allocator) {
    }

    NodeTypeId type;
    uint32 schemaVersion = 0;
    PropertyRecord properties;
    Array<PinKeyRename> renames;
};

// 옛 버전 노드를 다음 버전으로 옮긴다(GD13, 10.1절). 상태 없이 같은 입력에 같은 결과를 낸다.
// 옮길 수 없으면 실패를 돌려준다(그 노드는 옛 버전 그대로 남고 보고서에 적힌다).
using MigrateFn = Result<void> (*)(const NodeMigrationInput& input,
                                   NodeMigrationResult& out) noexcept;

// 노드 종류 하나의 서술이다(6.2절).
struct NodeTypeDescriptor {
    // "<도메인>.<이름>" 형식을 권한다(예: "Material.Multiply"). "Graph." 접두는 공통 종류 전용이다.
    Name type;
    // 서술 버전이다. 1 이상. 1보다 크면 migrate가 있어야 한다(등록 검사).
    uint32 schemaVersion = 1;
    // 속성 struct ID(Record schema)다. 0이면 속성이 없다(GI13).
    Guid propertiesStruct;
    Span<const PinDescriptor> staticPins;
    // 선택이다.
    DynamicPinsFn dynamicPins = nullptr;
    // 선택이다. 입력은 정적 입력 핀, 출력은 정적 출력 핀이고 출력마다 한 번이다.
    Span<const PassThroughPair> passThrough;
    NodeMenuInfo menu;
    NodeCapabilities capabilities = DefaultNodeCapabilities;
    // 옛 버전 노드를 옮기는 함수다(GD13). schemaVersion이 1이면 선택이다.
    MigrateFn migrate = nullptr;
};

} // namespace rocks::nodegraph
