#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <NodeGraph/GraphSchema.h>
#include <NodeGraph/NodeTypeDescriptor.h>

// 그래프 schema·노드 종류 등록표다(Docs/Design/NodeGraph.ko.md 6.2·10절, GD11).
//
// 도메인마다 schema 하나와 노드 종류 여럿을 module lease와 함께 등록한다. 공통 노드 종류(지금은
// 재경유 Graph.Reroute)는 Initialize가 모든 도메인에 대해 등록한다. 조회는 공통 종류가 먼저다.
//
// 사용 예:
//   NodeTypeRegistry registry(allocator, names);
//   if (!registry.Initialize()) { ... }
//   auto schema = registry.RegisterSchema(materialSchema, lease);
//   auto multiply = registry.RegisterNodeType(materialSchema.Domain(), multiplyDescriptor, lease);
//   PinList pins(allocator);
//   registry.CollectPins(document, graph, node, pins);
//
// 스레드: NG0은 소유 lane 하나가 쓰는 단일 스레드 객체다(동기화 없음). 해제 drain·여러 스레드 조회·
// NodeTypesChanged는 NG3에서 정한다(14절).
// 수명: allocator·names는 등록표보다 오래 살아야 한다. 소멸하면 남은 등록의 lease를 놓는다.
namespace rocks::nodegraph {

// 공통 노드 종류의 이름 접두다. 도메인은 이 접두로 시작하는 종류를 등록할 수 없다(ASCII 대소문자
// 무시).
inline constexpr StringView CommonNodeTypePrefix = "Graph.";

// 재경유 노드 종류와 그 핀 키다(6.1절).
inline constexpr StringView RerouteNodeTypeName = "Graph.Reroute";
inline constexpr StringView ReroutePinIn = "In";
inline constexpr StringView ReroutePinOut = "Out";

// 지금 재경유 서술 버전이다.
inline constexpr uint32 RerouteSchemaVersion = 1;

// 등록 하나의 식별이다. 0은 없음이다.
struct SchemaRegistrationId {
    uint64 value = 0;
};

struct NodeTypeRegistrationId {
    uint64 value = 0;
};

// 등록된 노드 종류다. descriptor의 staticPins·passThrough는 등록표가 소유한 사본을 가리킨다.
struct RegisteredNodeType {
    // 공통 종류면 None이다.
    Name domain;
    NodeTypeDescriptor descriptor;
};

class ROCKS_NODEGRAPH_API NodeTypeRegistry {
public:
    NodeTypeRegistry(Allocator& allocator, NamePool& names) noexcept;
    ~NodeTypeRegistry() noexcept;

    NodeTypeRegistry(const NodeTypeRegistry&) = delete;
    NodeTypeRegistry& operator=(const NodeTypeRegistry&) = delete;
    NodeTypeRegistry(NodeTypeRegistry&&) = delete;
    NodeTypeRegistry& operator=(NodeTypeRegistry&&) = delete;

    // 공통 종류의 이름을 names에 intern하고 공통 종류를 등록한다. 다른 함수보다 먼저 한 번 부른다.
    // 두 번째 호출은 AlreadyExists다. intern·할당 오류를 그대로 돌려준다(실패하면 등록표는 비어
    // 있다).
    Result<void> Initialize() noexcept;

    // 도메인 schema를 등록한다. schema·lease는 해제까지 보관한다.
    //
    // 오류: Initialize 전 Unsupported, 도메인이 None InvalidArgument, 이미 그 도메인 schema가
    // 있으면 AlreadyExists, 할당 실패.
    Result<SchemaRegistrationId> RegisterSchema(const GraphSchema& schema,
                                                ModuleLease lease) noexcept;

    // schema 등록을 해제한다. 그 도메인의 노드 종류가 남아 있으면 Busy, 없는 id면 NotFound다.
    Result<void> UnregisterSchema(SchemaRegistrationId id) noexcept;

    // domain에 노드 종류를 등록한다. 핀·통과 쌍 배열은 소유 복사한다(10절).
    //
    // 검사(InvalidArgument): 종류 이름이 None이거나 CommonNodeTypePrefix로 시작함, schemaVersion 0,
    // 정적 핀 수가 MaxPinsPerNode 초과, 핀 키 None·중복, 핀 타입이 UnresolvedPinType도 schema 표
    // 색인도 아님, maxEdges 0, 방향·종류·값 정책이 정의되지 않은 값, 입력 핀의 ordered, 통과 쌍이
    // 정적 입력·출력 핀이 아니거나 같은 출력 중복, 정의되지 않은 capability 비트. 그 밖의 오류:
    // Initialize 전 Unsupported, 도메인 schema 없음 NotFound, 같은 종류가 이미 있음 AlreadyExists,
    // names·할당 오류. 실패하면 등록표는 그대로다.
    Result<NodeTypeRegistrationId>
    RegisterNodeType(Name domain, const NodeTypeDescriptor& descriptor, ModuleLease lease) noexcept;

    // 노드 종류 등록을 해제한다(lease를 놓는다). 없는 id면 NotFound다. drain·통지는 NG3에서 더한다.
    Result<void> UnregisterNodeType(NodeTypeRegistrationId id) noexcept;

    // domain의 schema다. 없으면 nullptr이다.
    const GraphSchema* FindSchema(Name domain) const noexcept;

    // 노드 종류다(공통 종류 먼저, 그다음 domain). 없으면 nullptr이다. 포인터는 다음 등록·해제까지
    // 유효하다.
    const RegisteredNodeType* FindNodeType(Name domain, Name type) const noexcept;

    // node의 현재 핀 목록(정적 핀 + 동적 핀)을 out에 채운다(out은 먼저 비운다).
    //
    // 오류: 등록되지 않은 종류 NotFound, 동적 핀 함수의 오류, 동적 핀의 키 None·중복과 정적 핀 규칙
    // 위반(RegisterNodeType의 핀 검사, 타입 색인은 문서 도메인 schema 표) InvalidArgument,
    // MaxPinsPerNode 초과 CapacityExceeded, 할당 실패.
    Result<void> CollectPins(const GraphDocument& document, const Graph& graph,
                             const GraphNode& node, PinList& out) const noexcept;

    // 재경유 종류 이름 토큰이다(Initialize 뒤).
    Name RerouteType() const noexcept {
        return rerouteType_;
    }

    bool IsReroute(Name type) const noexcept {
        return initialized_ && type == rerouteType_;
    }

    NamePool& Names() const noexcept {
        return names_;
    }

    Allocator& GetAllocator() const noexcept {
        return allocator_;
    }

private:
    struct SchemaEntry {
        SchemaRegistrationId id;
        const GraphSchema* schema = nullptr;
        Name domain;
        ModuleLease lease;
    };

    struct NodeTypeEntry {
        explicit NodeTypeEntry(Allocator& allocator) noexcept
            : pins(allocator), passThrough(allocator) {
        }

        NodeTypeRegistrationId id;
        RegisteredNodeType registered;
        // registered.descriptor의 staticPins·passThrough가 가리키는 소유 사본이다. Array 이동은
        // 저장소를 옮기지 않으므로 항목이 옮겨져도 Span이 유효하다.
        Array<PinDescriptor> pins;
        Array<PassThroughPair> passThrough;
        ModuleLease lease;
    };

    Result<void> ValidateDescriptor(const GraphSchema* schema,
                                    const NodeTypeDescriptor& descriptor) const noexcept;
    Result<NodeTypeRegistrationId> AddNodeType(Name domain, const NodeTypeDescriptor& descriptor,
                                               ModuleLease lease) noexcept;
    const RegisteredNodeType* LookupNodeType(Name domain, Name type) const noexcept;
    void RebuildTypeLookup() noexcept;
    void AddTypeLookup(Name domain, Name type, uint32 index) noexcept;

    Allocator& allocator_;
    NamePool& names_;
    Array<SchemaEntry> schemas_;
    Array<NodeTypeEntry> nodeTypes_;
    // (도메인, 종류) 토큰 hash → nodeTypes_ 색인이다. 빗나가거나 충돌하면 선형 탐색으로
    // 찾는다(정확성은 표와 무관하다).
    HashMap<uint64, uint32> typeLookup_;
    // 표가 모든 종류를 담고 충돌이 없는가(참이면 빗나간 조회는 없음이다).
    bool typeLookupComplete_ = true;
    Name rerouteType_;
    Name reroutePinIn_;
    Name reroutePinOut_;
    PinDescriptor reroutePins_[2];
    uint64 nextId_ = 1;
    bool initialized_ = false;
};

} // namespace rocks::nodegraph
