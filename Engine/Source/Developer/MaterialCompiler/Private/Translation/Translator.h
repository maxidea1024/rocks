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
#include <MaterialCompiler/MaterialTargetField.h>
#include <MaterialCompiler/MaterialTranslationInput.h>
#include <MaterialSchema/MaterialNodeDefinition.h>

#include "AttributeEntry.h"
#include "CustomOutputKind.h"
#include "LoweringTable.h"
#include "SurfaceRecord.h"

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

// 머티리얼 하나가 참조할 수 있는 파라미터 컬렉션 수다(9.7절 — UE
// MaxNumParameterCollectionsPerMaterial, RE/Public/ParameterCollection.h:13–18).
inline constexpr usize MaxMaterialCollections = 2;

// post process 머티리얼 하나가 읽을 수 있는 UserSceneTexture 수다(UE
// kPostProcessMaterialInputCountMax — Renderer/Public/PostProcess/PostProcessMaterialInputs.h:15).
inline constexpr usize MaxUserSceneTextures = 5;

// 첫 UserSceneTexture의 scene texture id다(UE PPI_UserSceneTexture0 — MaterialSceneTextureId.h:80).
inline constexpr uint32 MaterialUserSceneTextureFirstId = 32;

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

    // RVT 출력 노드 입력을 번역하는 중인가(6.11절 MS2d3 RVT 범위 — 픽셀 문맥, 결과 캐시는 따로).
    bool VirtualTextureScope() const noexcept {
        return virtualTextureScope_;
    }

    // 지금 번역 중인 평가 문맥이다.
    EvaluationContext Context() const noexcept {
        return context_;
    }

    // context 문맥에서 consumer 노드 입력 pin의 값이다(VertexInterpolator가 입력을 정점 문맥으로
    // 번역한다 — 6.9절 MS2b4a). 끝나면 지금 문맥으로 돌아온다.
    Result<IrValueId> LowerInContext(EvaluationContext context,
                                     const nodegraph::GraphNode& consumer, Name pin,
                                     bool& connected) noexcept;

    // 픽셀 문맥이 TexCoord(index)를 읽었다(CustomizedUv 수 — 6.9절 MS2b4a).
    void NotePixelTexCoord(uint32 index) noexcept {
        pixelTexCoords_ = index + 1 > pixelTexCoords_ ? index + 1 : pixelTexCoords_;
    }

    // 번역 대상이다. field를 읽은 항목으로 기록한다(6.9절 MS2b3).
    const MaterialTranslationTarget& Target(MaterialTargetField field) noexcept {
        out_.consultedTarget |= ToMask(field);
        return input_.target;
    }

    // 자기 샘플러를 쓰는 텍스처 slot(출처, id)을 센다. 이미 센 slot이면 그대로 참이고, 번역 대상
    // maxSamplers를 넘으면 넣지 않고 거짓이다(6.10절 MS2c3 V9). count는 넣은 뒤의 수다.
    Result<bool> ReferenceSampler(uint8 source, Guid id, uint32& count) noexcept;

    // UserSceneTexture 이름을 번호로 바꾼다(처음 참조 순). 상한을 넘으면 거짓이다(6.10절 MS2c4).
    Result<bool> ReferenceUserSceneTexture(StringView name, uint32& index) noexcept;

    // 표면 기록 하나를 더하고 그 표면 값(IR SurfaceRef)을 돌려준다(6.11절 MS2d1).
    Result<IrValueId> AddSurface(const SurfaceRecord& record) noexcept;
    // 속성 기록 하나를 더하고 그 속성 집합 값(IR AttributesRef)을 돌려준다(6.12절 MS2e1). 같은
    // Guid가 둘이면 뒤의 것이 이긴다.
    Result<IrValueId> AddAttributes(Span<const AttributeEntry> entries) noexcept;
    // 속성 집합 값의 칸을 out에 복사한다(아니면 false).
    Result<bool> CopyAttributes(IrValueId value, Array<AttributeEntry>& out) const noexcept;
    // 속성 정의다(내장 → 등록 표 순, 없으면 nullptr).
    const MaterialAttributeDefinition* FindAttribute(Guid id) const noexcept;
    const MaterialAttributeDefinition* FindAttributeByName(StringView name) const noexcept;
    // 표면 값의 기록이다. 표면 값이 아니면 nullptr이다.
    const SurfaceRecord* FindSurface(IrValueId value) const noexcept;

    // value가 결과에 닿을 때만 낼 오류를 둔다(지연 진단 — 6.10절 MS2c4). message는 번역이 끝날
    // 때까지 살아 있어야 한다(문자열 상수).
    Result<void> DeferError(IrValueId value, MaterialDiagnosticCode code, Guid node,
                            StringView message) noexcept;

    // 컬렉션 asset을 참조 목록에 넣는다. 이미 있으면 그대로 참이고, 상한을 넘으면 넣지 않고
    // 거짓이다.
    Result<bool> ReferenceCollection(Guid asset) noexcept;

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
        Failed = 2,
        // 늦은 진단 노드다(TODO·모르는 노드 — 출력 핀마다 Pending 자리 값, 6.10절 MS2c1).
        Pending = 3
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
    // 출력 노드 Attributes 핀의 칸이다(연결이 없으면 비어 있다 — 6.12절 MS2e1).
    Result<void> AttributesFromOutput(LoweringContext& context,
                                      Array<AttributeEntry>& entries) noexcept;
    // CustomizedUv 출력 핀의 정점 결과다(픽셀 문맥 뒤 — 6.9절 MS2b4a).
    Result<void> TranslateCustomizedUvs(const nodegraph::GraphNode& output) noexcept;
    // 보간 성분 수 V9 검사다(모듈을 만든 뒤).
    Result<void> CheckInterpolantBudget() noexcept;
    // 출력 노드 Surface 핀의 표면을 접어 Surface·Normal 결과로 내보낸다(6.11절 MS2d1 — 픽셀 문맥).
    Result<void> TranslateSurface(LoweringContext& context) noexcept;
    // custom 출력 노드와 표면이 주는 custom 출력의 결과·정의다(6.11절 MS2d3 — 출력 노드 번역 뒤).
    Result<void> TranslateCustomOutputs() noexcept;
    // 노드 하나의 custom 출력 결과다(빈도의 문맥마다 그 입력을 번역한다).
    Result<void> TranslateCustomOutput(const nodegraph::GraphNode& node,
                                       const CustomOutputKind& kind) noexcept;
    // 출력 노드 속성 집합의 custom 속성 칸이 주는 출력이다(6.12절 MS2e1).
    Result<void> AttributeCustomOutputs(Span<const bool> present) noexcept;
    // 노드가 없는 종류에 뿌리 표면이 주는 출력이다.
    Result<void> SurfaceCustomOutputs(Span<const bool> present) noexcept;
    // 노드 속성이 정하는 정의를 모은다(VolumetricAdvanced·EmptySpaceSkipping).
    Result<void> CustomOutputDefines(const nodegraph::GraphNode& node,
                                     const CustomOutputKind& kind) noexcept;
    // 대표 노멀 경로에 PixelNormal이 닿으면 순환 오류다(6.11절 — UE HT:11784–11787보다 넓다).
    Result<void> CheckNormalCycle() noexcept;
    // 결과에서 닿는 값에 묶인 지연 진단을 낸다(6.10절 MS2c4).
    Result<void> CheckDeferred() noexcept;
    // 결과에서 닿는 Pending 자리 값의 늦은 진단이다(6.10절 MS2c1 — DCE 뒤, 조건 값 가지는 경고).
    Result<void> CheckPending() noexcept;
    // node의 pin 출력 자리 값을 만든다(처음 요청 때).
    Result<IrValueId> PendingOutput(const nodegraph::GraphNode& node, Name pin) noexcept;

    HashMap<PinKey, IrValueId, PinKeyHash>& Outputs() noexcept {
        return context_ == EvaluationContext::Vertex           ? outputsVertex_
               : context_ == EvaluationContext::VertexPrevious ? outputsPrevious_
               : virtualTextureScope_                          ? outputsVirtualTexture_
                                                               : outputsPixel_;
    }

    HashMap<Guid, NodeState>& States() noexcept {
        return context_ == EvaluationContext::Vertex           ? statesVertex_
               : context_ == EvaluationContext::VertexPrevious ? statesPrevious_
               : virtualTextureScope_                          ? statesVirtualTexture_
                                                               : statesPixel_;
    }

    const HashMap<Guid, NodeState>& States() const noexcept {
        return context_ == EvaluationContext::Vertex           ? statesVertex_
               : context_ == EvaluationContext::VertexPrevious ? statesPrevious_
               : virtualTextureScope_                          ? statesVirtualTexture_
                                                               : statesPixel_;
    }
    // node가 기대는 노드들이다(들어오는 간선의 출발 노드, 이름 있는 재경유의 선언).
    Result<void> CollectDependencies(usize node, Array<Dependency>& out) noexcept;
    // 출력 노드 roots 핀(활성 핀)에 들어오는 간선의 출발 노드다(간선 저장 순서).
    Result<void> RootStarts(const nodegraph::GraphNode& output, Span<const Name> roots,
                            Array<usize>& starts) noexcept;
    // starts에서 닿는 노드의 의존 후위 순서를 만들고 순환 간선을 진단한다. 이미 번역한 노드는
    // 건너뛰고, 번역 중인 노드에 닿으면 순환이다(늦은 입력 계획 — 6.9절 MS2b3).
    Result<void> PlanOrder(Span<const usize> starts, Array<usize>& order) noexcept;
    // order대로 아직 번역하지 않은 노드를 번역한다.
    Result<void> LowerPlanned(Span<const usize> order) noexcept;
    // consumer 노드의 입력 pin이 계획에 들어가는가(늦은 입력이 아니다).
    bool IsEagerInput(const nodegraph::GraphNode& consumer, Name pin) const noexcept;
    Result<void> LowerNodeOnce(const nodegraph::GraphNode& node) noexcept;

    Allocator& allocator_;
    const MaterialTranslationInput& input_;
    MaterialTranslation& out_;
    IrBuilder builder_;
    const nodegraph::Graph* graph_ = nullptr;
    HashMap<Guid, usize> nodes_;
    HashMap<PinKey, usize, PinKeyHash> edges_;
    // 노드 결과·상태는 평가 문맥마다다(6.9절 — 문맥을 바꿔도 지우지 않는다, MS2b4a).
    HashMap<PinKey, IrValueId, PinKeyHash> outputsVertex_;
    HashMap<PinKey, IrValueId, PinKeyHash> outputsPrevious_;
    HashMap<PinKey, IrValueId, PinKeyHash> outputsPixel_;
    HashMap<Guid, NodeState> statesVertex_;
    HashMap<Guid, NodeState> statesPrevious_;
    HashMap<Guid, NodeState> statesPixel_;
    // RVT 범위(픽셀 문맥)의 노드 결과·상태다(6.11절 MS2d3).
    HashMap<PinKey, IrValueId, PinKeyHash> outputsVirtualTexture_;
    HashMap<Guid, NodeState> statesVirtualTexture_;
    bool virtualTextureScope_ = false;
    // 픽셀 문맥이 쓴 TexCoord의 최대 상수 색인 + 1이다(CustomizedUv 수 — 6.9절 MS2b4a).
    uint32 pixelTexCoords_ = 0;
    HashMap<Name, LoweringFn> lowerings_;
    // 늦은 입력 노드 종류 → 계획에 넣는 입력 키 원문이다.
    HashMap<Name, Span<const StringView>> lazyNodes_;
    // 노드 색인마다 들어오는 간선 색인이다(CSR — incomingStart_[i]…incomingStart_[i + 1]).
    Array<usize> incomingStart_;
    Array<usize> incoming_;
    // 순환으로 끊은 (노드, 입력 핀)이다(V2는 계획 때 한 번 진단했다).
    HashMap<PinKey, bool, PinKeyHash> cutInputs_;
    Name namedRerouteUsage_;
    Array<IrResult> results_;
    // 참조한 파라미터 컬렉션이다(처음 참조 순 — 상한 검사용, 모듈 표는 asset id 순으로 따로
    // 만든다).
    Array<Guid> collections_;
    // 자기 샘플러를 쓰는 텍스처 slot이다(상한 검사용 — 출처 | id).
    struct SamplerSlot {
        uint8 source = 0;
        Guid id;
    };

    Array<SamplerSlot> samplers_;

    // 지연 진단 하나다.
    struct DeferredError {
        IrValueId value = NoIrValue;
        MaterialDiagnosticCode code = MaterialDiagnosticCode::UnsupportedCapability;
        Guid node;
        StringView message;
    };

    Array<DeferredError> deferred_;
    // 표면 기록이다(SurfaceRef 즉시값 [0]이 번호 — 6.11절 MS2d1).
    Array<SurfaceRecord> surfaces_;
    // 속성 기록이다(AttributesRef 즉시값 [0]이 번호 — 칸은 attributeEntries_[first, first + count),
    // 6.12절 MS2e1).
    struct AttributeRange {
        uint32 first = 0;
        uint32 count = 0;
    };

    Array<AttributeEntry> attributeEntries_;
    Array<AttributeRange> attributeRanges_;
    // 출력 노드 속성 집합의 픽셀 칸이다(useMaterialAttributes — 표면·custom 출력이 읽는다).
    Array<AttributeEntry> attributeOutputs_;
    // 내보낸 shading model 집합 비트다(Renderer 14.2절 번호).
    uint32 shadingModels_ = 0;
    // 내보낸 뿌리 표면이다(추가 칸 — 표면이 주는 custom 출력, 6.11절 MS2d3).
    SurfaceRecord exportedSurface_;
    // 노드 속성이 정하는 생성 원문 정의다.
    Array<IrDefine> defines_;
    // 출력 노드다(Run 동안).
    const nodegraph::GraphNode* outputNode_ = nullptr;
    Name rerouteType_;
    Name rerouteIn_;
    Name rerouteOut_;
    uint32 depth_ = 0;
    uint32 errorCount_ = 0;
    EvaluationContext context_ = EvaluationContext::Vertex;
    bool failed_ = false;
};

} // namespace rocks::material::detail
