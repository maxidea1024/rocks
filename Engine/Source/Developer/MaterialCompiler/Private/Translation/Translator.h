#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Hash.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialDiagnosticCode.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <MaterialCompiler/MaterialIrModule.h>
#include <MaterialCompiler/MaterialTranslation.h>
#include <MaterialCompiler/MaterialTranslationInput.h>
#include <MaterialSchema/MaterialNodeDefinition.h>

#include "LoweringTable.h"

// 그래프 → IR 번역기 본체다(Docs/Design/MaterialSystem 6.3·6.8절). 먼저 출력 노드의 활성 핀에서
// 간선을 거슬러 닿는 노드를 반복(스택 없는 재귀가 아닌) 깊이 우선으로 훑어 의존 후위 순서를 만든다.
// 그 순서로 노드마다 lowering 함수를 한 번 부르므로, 함수가 요청하는 입력은 늘 이미 번역된 값이다
// (그래프 깊이가 C++ 스택을 쓰지 않는다). 훑는 중 회색 노드를 다시 만나면 순환(V2)이고 그 간선은
// 끊긴 값(NoIrValue)이다. lowering 함수는 LoweringContext로 이 객체에 입력 값을 요청한다.
//
// 오류 전파: 그래프 오류는 진단으로 남기고 값 자리에 NoIrValue를 돌려준다. NoIrValue를 받은 연산은
// 다시 진단하지 않고 NoIrValue를 낸다(UE 번역기의 INDEX_NONE과 같다). Result 실패는 할당 같은
// 시스템 오류뿐이다.
//
// 평가 문맥(6.9절): 정점·이전 정점·픽셀 문맥마다 그 진입점의 활성 출력 핀에서 따로 번역한다. 노드
// 결과는 문맥마다 기억하고(UE 셰이더 단계별 함수 상태·이전 프레임 키와 같다), IR 값은 문맥 사이에
// 공유한다. 같은 (코드, 노드, 핀, 문구) 진단은 한 번만 남는다.
namespace rocks::material::detail {

// 평가 문맥이다(6.9절 — 번역 순서와 같다).
enum class EvaluationContext : uint8 {
    // Vertex 진입점(UE SF_Vertex).
    Vertex = 0,
    // VertexPrevious 진입점(UE bCompilingPreviousFrame).
    VertexPrevious = 1,
    // 픽셀 진입점들(UE SF_Pixel).
    Pixel = 2
};

// 순서 계획 밖에서 노드를 재귀로 번역할 때(의존을 미리 알 수 없는 요청)의 깊이 상한이다(6.8절
// GraphTooDeep). 계획된 번역은 재귀하지 않는다.
inline constexpr uint32 MaxLoweringDepth = 256;

class Translator {
public:
    Translator(Allocator& allocator, const MaterialTranslationInput& input,
               MaterialTranslation& out) noexcept;

    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    // 번역 전체다(색인 → V8 → 출력 핀 → 진입점 → V4 → 모듈).
    Result<void> Run() noexcept;

    // consumer 노드 입력 pin에 연결된 값이다. 연결이 없으면(비활성 노드가 통과 쌍 없이 끊은 경우
    // 포함) connected가 거짓이고 NoIrValue다.
    Result<IrValueId> LowerConnection(const nodegraph::GraphNode& consumer, Name pin,
                                      bool& connected) noexcept;

    // node의 출력 pin 값이다(처음이면 노드를 lowering한다).
    Result<IrValueId> LowerNodeOutput(const nodegraph::GraphNode& node, Name pin) noexcept;

    // 주 그래프의 노드다. 없으면 nullptr이다.
    const nodegraph::GraphNode* FindNode(Guid id) const noexcept;

    // consumer 노드 입력 pin에 실제 연결이 있는가. 비활성 노드는 통과 쌍을 따라가고 쌍이 없으면
    // 끊긴 것이다(NodeGraph 6.5절).
    bool HasConnection(const nodegraph::GraphNode& consumer, Name pin) const noexcept;

    // (node, pin) 입력·의존이 계획 때 순환(V2)으로 끊겼는가(pin None은 간선 없는 의존 — 이름 있는
    // 재경유).
    bool IsCut(Guid node, Name pin) const noexcept;

    // 노드 출력 하나를 기록한다(같은 핀을 두 번 기록하면 InvalidArgument — lowering 함수의 잘못).
    Result<void> RecordOutput(Guid node, Name pin, IrValueId value) noexcept;

    // 진단 하나다. 오류면 번역이 실패로 끝난다.
    Result<void> Report(nodegraph::GraphDiagnosticSeverity severity, MaterialDiagnosticCode code,
                        Guid node, Name pin, StringView message) noexcept;

    IrBuilder& Builder() noexcept {
        return builder_;
    }

    NamePool& Names() noexcept {
        return input_.names;
    }

    Allocator& GetAllocator() noexcept {
        return allocator_;
    }

    const MaterialTranslationInput& Input() const noexcept {
        return input_;
    }

    // 번역 중인 주 그래프다(Run이 찾은 뒤).
    const nodegraph::Graph& Graph() const noexcept {
        return *graph_;
    }

    // 지금 번역 중인 평가 문맥이다.
    EvaluationContext Context() const noexcept {
        return context_;
    }

    // 종류 이름의 lowering 함수다. 없으면 nullptr이다.
    LoweringFn FindLowering(Name type) const noexcept;

private:
    struct PinKey {
        Guid node;
        Name pin;

        friend bool operator==(const PinKey& left, const PinKey& right) noexcept {
            return left.node == right.node && left.pin == right.pin;
        }
    };

    struct PinKeyHash {
        uint64 operator()(const PinKey& key) const noexcept {
            return HashCombine(Hash<Guid>{}(key.node), Hash<Name>{}(key.pin));
        }
    };

    enum class NodeState : uint8 {
        Visiting = 0,
        // 오류 없이 번역했다.
        Done = 1,
        // 번역 중 오류를 진단했다(없는 출력 핀 값을 다시 진단하지 않는다).
        Failed = 2
    };

    // 의존 하나다: consumer 노드의 input 핀이 source 노드에 기댄다.
    struct Dependency {
        usize source = 0;
        PinKey input;
    };

    Result<void> BuildIndex() noexcept;
    Result<void> CheckParameterNames() noexcept;
    // 지금 문맥의 진입점 결과를 출력 노드 핀에서 만든다(대기 핀·비활성 핀 진단은 문맥마다 같고
    // 한 번만 남는다).
    Result<void> TranslateOutputs(const nodegraph::GraphNode& output) noexcept;
    // 지금 문맥의 활성 출력 핀이다.
    Result<void> ContextRoots(Array<Name>& roots) const noexcept;
    // V4(단계 — 미분·문맥 값 단계 집합)·V6(문맥 값 영역) 검사다(6.9절).
    Result<void> CheckContextValues() noexcept;
    // node가 기대는 노드들이다(들어오는 간선의 출발 노드, 이름 있는 재경유의 선언).
    Result<void> CollectDependencies(usize node, Array<Dependency>& out) noexcept;
    // 출력 노드의 roots 핀(활성 핀)에서 닿는 노드의 의존 후위 순서를 만들고 순환 간선을 진단한다.
    Result<void> PlanOrder(const nodegraph::GraphNode& output, Span<const Name> roots,
                           Array<usize>& order) noexcept;
    Result<void> LowerNodeOnce(const nodegraph::GraphNode& node) noexcept;

    Allocator& allocator_;
    const MaterialTranslationInput& input_;
    MaterialTranslation& out_;
    IrBuilder builder_;
    const nodegraph::Graph* graph_ = nullptr;
    HashMap<Guid, usize> nodes_;
    HashMap<PinKey, usize, PinKeyHash> edges_;
    HashMap<PinKey, IrValueId, PinKeyHash> outputs_;
    HashMap<Guid, NodeState> states_;
    HashMap<Name, LoweringFn> lowerings_;
    // 노드 색인마다 들어오는 간선 색인이다(CSR — incomingStart_[i]…incomingStart_[i + 1]).
    Array<usize> incomingStart_;
    Array<usize> incoming_;
    // 순환으로 끊은 (노드, 입력 핀)이다(V2는 계획 때 한 번 진단했다).
    HashMap<PinKey, bool, PinKeyHash> cutInputs_;
    Name namedRerouteUsage_;
    Array<IrResult> results_;
    Name rerouteType_;
    Name rerouteIn_;
    Name rerouteOut_;
    uint32 depth_ = 0;
    uint32 errorCount_ = 0;
    EvaluationContext context_ = EvaluationContext::Vertex;
    bool failed_ = false;
};

} // namespace rocks::material::detail
