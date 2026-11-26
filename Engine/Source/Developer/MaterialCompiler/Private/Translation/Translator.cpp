#include "Translator.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/MaterialGraphTypes.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <NodeGraph/NodeTypeRegistry.h>

#include "LoweringContext.h"

// 그래프 → IR 번역기 본체다(Docs/Design/MaterialSystem 6.8절).
namespace rocks::material::detail {

namespace {

// 출력 노드의 MS2a 핀 하나다(6.8절 "출력 노드 MS2a 핀과 진입점").
struct OutputSlot {
    StringView pin;
    IrEntry entry;
    MaterialPinType type;
};

// 진입점 순서(7.3절)대로다. 같은 진입점 안은 핀 순서다.
constexpr OutputSlot OutputSlots[] = {
    {"WorldPositionOffset", IrEntry::Vertex, MaterialPinType::Float3},
    {"AmbientOcclusion", IrEntry::Surface, MaterialPinType::Float1},
    {"Refraction", IrEntry::Surface, MaterialPinType::Float3},
    {"SurfaceThickness", IrEntry::Surface, MaterialPinType::Float1},
    {"Opacity", IrEntry::Opacity, MaterialPinType::Float1},
    {"OpacityMask", IrEntry::OpacityMask, MaterialPinType::Float1},
    {"PixelDepthOffset", IrEntry::PixelDepthOffset, MaterialPinType::Float1},
};

// MS2a가 번역하지 않는 출력 핀이다(연결이 있으면 NotYetLowered — Surface·Attributes·Displacement는
// MS2d, CustomizedUv는 MS2b).
constexpr StringView PendingOutputPins[] = {
    "Surface",       "Attributes",    "Displacement",  "CustomizedUv0",
    "CustomizedUv1", "CustomizedUv2", "CustomizedUv3", "CustomizedUv4",
    "CustomizedUv5", "CustomizedUv6", "CustomizedUv7",
};

// 영역의 진단용 영어 이름이다(V6 문구).
StringView DomainText(MaterialDomain domain) noexcept {
    switch (domain) {
    case MaterialDomain::Surface:
        return "Surface";
    case MaterialDomain::DeferredDecal:
        return "DeferredDecal";
    case MaterialDomain::LightFunction:
        return "LightFunction";
    case MaterialDomain::Volume:
        return "Volume";
    case MaterialDomain::PostProcess:
        return "PostProcess";
    case MaterialDomain::UserInterface:
        return "UserInterface";
    }

    return "unknown";
}

// 진입점이 정점 문맥 둘 중 하나인가.
bool IsVertexEntry(IrEntry entry) noexcept {
    return entry == IrEntry::Vertex || entry == IrEntry::VertexPrevious;
}

// 출력 슬롯 진입점이 문맥에 속하는가(정점 슬롯은 정점·이전 정점 문맥 둘 다 — 6.9절).
bool SlotInContext(IrEntry entry, EvaluationContext context) noexcept {
    return context == EvaluationContext::Pixel ? !IsVertexEntry(entry) : IsVertexEntry(entry);
}

bool IsTranslucent(BlendMode mode) noexcept {
    return mode != BlendMode::Opaque && mode != BlendMode::Masked;
}

// UE Substrate 경로 IsPropertyActive_Internal(RE/Private/Materials/Material.cpp)의 MS2a 핀
// 부분이다.
bool IsOutputActive(StringView pin, const MaterialTraitsDesc& traits) noexcept {
    const MaterialDomain domain = traits.domain;
    const BlendMode blend = traits.blendMode;
    const bool surface = domain == MaterialDomain::Surface;
    if (pin == StringView("WorldPositionOffset")) {
        return surface || domain == MaterialDomain::DeferredDecal ||
               domain == MaterialDomain::UserInterface;
    }

    if (pin == StringView("Opacity")) {
        return blend == BlendMode::AlphaComposite &&
               (surface || domain == MaterialDomain::DeferredDecal ||
                domain == MaterialDomain::PostProcess || domain == MaterialDomain::UserInterface);
    }

    if (pin == StringView("OpacityMask")) {
        return blend == BlendMode::Masked && (surface || domain == MaterialDomain::UserInterface);
    }

    if (pin == StringView("PixelDepthOffset")) {
        return surface && (!IsTranslucent(blend) || traits.outputTranslucentVelocity);
    }

    if (pin == StringView("AmbientOcclusion")) {
        return surface || domain == MaterialDomain::DeferredDecal;
    }

    if (pin == StringView("Refraction")) {
        return surface && IsTranslucent(blend) && blend != BlendMode::AlphaHoldout &&
               blend != BlendMode::Modulate && traits.usesDistortion &&
               traits.refractionMethod != RefractionMethod::IndexOfRefractionFromF0;
    }

    if (pin == StringView("SurfaceThickness")) {
        return surface && traits.thinSurface;
    }

    return false;
}

} // namespace

Translator::Translator(Allocator& allocator, const MaterialTranslationInput& input,
                       MaterialTranslation& out) noexcept
    : allocator_(allocator), input_(input), out_(out), builder_(allocator), nodes_(allocator),
      edges_(allocator), outputs_(allocator), states_(allocator), lowerings_(allocator),
      incomingStart_(allocator), incoming_(allocator), cutInputs_(allocator), results_(allocator) {
}

const nodegraph::GraphNode* Translator::FindNode(Guid id) const noexcept {
    const usize* index = nodes_.Find(id);
    return index == nullptr ? nullptr : &graph_->nodes[*index];
}

bool Translator::IsCut(Guid node, Name pin) const noexcept {
    return cutInputs_.Contains(PinKey{node, pin});
}

bool Translator::HasConnection(const nodegraph::GraphNode& consumer, Name pin) const noexcept {
    const nodegraph::GraphNode* current = &consumer;
    Name currentPin = pin;
    // 비활성 노드 사슬을 따라간다. 사슬은 노드 수를 넘을 수 없으므로 그만큼에서 멈춘다(순환 보호).
    for (usize step = 0; step <= graph_->nodes.Size(); ++step) {
        const usize* edgeIndex = edges_.Find(PinKey{current->id, currentPin});
        if (edgeIndex == nullptr) {
            return false;
        }

        const nodegraph::GraphEdge& edge = graph_->edges[*edgeIndex];
        const nodegraph::GraphNode* source = FindNode(edge.fromNode);
        if (source == nullptr) {
            return false;
        }

        if (source->enableState != nodegraph::NodeEnableState::Disabled) {
            return true;
        }

        const MaterialNodeDefinition* definition = input_.catalog.Find(source->type);
        if (definition == nullptr) {
            return false;
        }

        bool passed = false;
        for (const MaterialPassThrough& pair : definition->passThrough) {
            auto output = input_.names.TryFind(pair.output);
            auto inputPin = input_.names.TryFind(pair.input);
            if (output && inputPin && output.Value() == edge.fromPin) {
                current = source;
                currentPin = inputPin.Value();
                passed = true;
                break;
            }
        }

        if (!passed) {
            return false;
        }
    }

    return false;
}

LoweringFn Translator::FindLowering(Name type) const noexcept {
    const LoweringFn* found = lowerings_.Find(type);
    return found == nullptr ? nullptr : *found;
}

Result<void> Translator::Report(nodegraph::GraphDiagnosticSeverity severity,
                                MaterialDiagnosticCode code, Guid node, Name pin,
                                StringView message) noexcept {
    if (severity == nodegraph::GraphDiagnosticSeverity::Error) {
        failed_ = true;
        ++errorCount_;
    }

    // 같은 (코드, 노드, 핀, 문구)는 한 번만 남긴다(여러 문맥에서 번역한 노드 — UE
    // HLSLMaterialTranslator.cpp:4446–4455). 오류 수는 세므로 노드는 그 문맥에서도 실패다.
    const uint32 diagnosticCode = ToDiagnosticCode(code);
    for (const nodegraph::GraphDiagnostic& item : out_.diagnostics.items) {
        if (item.code == diagnosticCode && item.node == node && item.pin == pin &&
            item.severity == severity && item.message.View() == message) {
            return Result<void>::Success();
        }
    }

    return out_.diagnostics.Add(severity, diagnosticCode, input_.asset, graph_->id, node, pin,
                                message);
}

Result<void> Translator::RecordOutput(Guid node, Name pin, IrValueId value) noexcept {
    auto inserted = outputs_.Insert(PinKey{node, pin}, value);
    if (!inserted) {
        return Result<void>::Failure(inserted.Error());
    }

    if (inserted.Value() == InsertOutcome::AlreadyPresent) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> Translator::BuildIndex() noexcept {
    for (usize index = 0; index < graph_->nodes.Size(); ++index) {
        if (auto added = nodes_.Insert(graph_->nodes[index].id, index); !added) {
            return Result<void>::Failure(added.Error());
        }
    }

    for (usize index = 0; index < graph_->edges.Size(); ++index) {
        const nodegraph::GraphEdge& edge = graph_->edges[index];
        if (auto added = edges_.Insert(PinKey{edge.toNode, edge.toPin}, index); !added) {
            return Result<void>::Failure(added.Error());
        }
    }

    // 들어오는 간선 인접 목록(CSR)이다. 간선 저장 순서를 그대로 둔다.
    if (auto sized = incomingStart_.TryResize(graph_->nodes.Size() + 1); !sized) {
        return sized;
    }

    for (usize& start : incomingStart_) {
        start = 0;
    }

    for (const nodegraph::GraphEdge& edge : graph_->edges) {
        if (const usize* target = nodes_.Find(edge.toNode)) {
            ++incomingStart_[*target + 1];
        }
    }

    for (usize index = 1; index < incomingStart_.Size(); ++index) {
        incomingStart_[index] += incomingStart_[index - 1];
    }

    if (auto sized = incoming_.TryResize(incomingStart_[graph_->nodes.Size()]); !sized) {
        return sized;
    }

    {
        SystemAllocator scratch;
        Array<usize> cursor(scratch);
        if (auto copied = cursor.TryAppend(incomingStart_.View()); !copied) {
            return copied;
        }

        for (usize index = 0; index < graph_->edges.Size(); ++index) {
            if (const usize* target = nodes_.Find(graph_->edges[index].toNode)) {
                incoming_[cursor[*target]++] = index;
            }
        }
    }

    // 문서에 없는 종류 이름은 pool에 없으므로(TryFind NotFound) 표에 넣지 않는다.
    const Span<const LoweringEntry> groups[] = {ConstantLowerings(), MathLowerings(),
                                                FlowLowerings(), ContextValueLowerings()};
    for (Span<const LoweringEntry> group : groups) {
        for (const LoweringEntry& entry : group) {
            auto name = input_.names.TryFind(entry.type);
            if (!name) {
                if (name.Error() == ErrorCode::NotFound) {
                    continue;
                }

                return Result<void>::Failure(name.Error());
            }

            if (auto added = lowerings_.Insert(name.Value(), entry.fn); !added) {
                return Result<void>::Failure(added.Error());
            }
        }
    }

    auto reroute = input_.names.TryFind(nodegraph::RerouteNodeTypeName);
    auto rerouteIn = input_.names.TryIntern(nodegraph::ReroutePinIn);
    if (!rerouteIn) {
        return Result<void>::Failure(rerouteIn.Error());
    }

    auto rerouteOut = input_.names.TryIntern(nodegraph::ReroutePinOut);
    if (!rerouteOut) {
        return Result<void>::Failure(rerouteOut.Error());
    }

    auto usage = input_.names.TryFind("Material.NamedRerouteUsage");
    rerouteType_ = reroute ? reroute.Value() : Name();
    rerouteIn_ = rerouteIn.Value();
    rerouteOut_ = rerouteOut.Value();
    namedRerouteUsage_ = usage ? usage.Value() : Name();
    return Result<void>::Success();
}

Result<IrValueId> Translator::LowerConnection(const nodegraph::GraphNode& consumer, Name pin,
                                              bool& connected) noexcept {
    connected = false;
    const usize* edgeIndex = edges_.Find(PinKey{consumer.id, pin});
    if (edgeIndex == nullptr) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (cutInputs_.Contains(PinKey{consumer.id, pin})) {
        // 계획 때 순환(V2)으로 진단하고 끊은 입력이다.
        connected = true;
        return Result<IrValueId>::Success(NoIrValue);
    }

    const nodegraph::GraphEdge& edge = graph_->edges[*edgeIndex];
    const nodegraph::GraphNode* source = FindNode(edge.fromNode);
    if (source == nullptr) {
        // 끊긴 간선은 문서 검증(GI2)이 막는다. 연결이 없는 것처럼 다룬다.
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (source->enableState == nodegraph::NodeEnableState::Disabled) {
        // 비활성 노드는 통과 쌍이 있으면 그 입력을, 없으면 연결이 없는 것처럼
        // 다룬다(NodeGraph 6.5절).
        const MaterialNodeDefinition* definition = input_.catalog.Find(source->type);
        if (definition == nullptr) {
            return Result<IrValueId>::Success(NoIrValue);
        }

        for (const MaterialPassThrough& pair : definition->passThrough) {
            auto output = input_.names.TryFind(pair.output);
            if (!output || !(output.Value() == edge.fromPin)) {
                continue;
            }

            auto inputPin = input_.names.TryFind(pair.input);
            if (!inputPin) {
                return Result<IrValueId>::Success(NoIrValue);
            }

            const NodeState* state = states_.Find(source->id);
            if (state != nullptr && *state == NodeState::Visiting) {
                if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                           MaterialDiagnosticCode::Cycle, source->id, edge.fromPin,
                                           "Data flow cycle through a disabled node");
                    !reported) {
                    return Result<IrValueId>::Failure(reported.Error());
                }

                connected = true;
                return Result<IrValueId>::Success(NoIrValue);
            }

            if (auto marked = states_.InsertOrAssign(source->id, NodeState::Visiting); !marked) {
                return Result<IrValueId>::Failure(marked.Error());
            }

            auto passed = LowerConnection(*source, inputPin.Value(), connected);
            states_.Erase(source->id);
            return passed;
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    connected = true;
    return LowerNodeOutput(*source, edge.fromPin);
}

Result<IrValueId> Translator::LowerNodeOutput(const nodegraph::GraphNode& node, Name pin) noexcept {
    if (const IrValueId* known = outputs_.Find(PinKey{node.id, pin})) {
        return Result<IrValueId>::Success(*known);
    }

    const NodeState* state = states_.Find(node.id);
    if (state == nullptr) {
        // 계획 밖의 요청이다(의존을 미리 알 수 없었다). 깊이 상한 안에서 재귀로 번역한다.
        if (depth_ >= MaxLoweringDepth) {
            if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                       MaterialDiagnosticCode::GraphTooDeep, node.id, pin,
                                       "Node chain is deeper than the translator limit");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }

        ++depth_;
        auto lowered = LowerNodeOnce(node);
        --depth_;
        if (!lowered) {
            return Result<IrValueId>::Failure(lowered.Error());
        }

        if (const IrValueId* value = outputs_.Find(PinKey{node.id, pin})) {
            return Result<IrValueId>::Success(*value);
        }

        state = states_.Find(node.id);
    }

    if (state != nullptr && *state == NodeState::Visiting) {
        if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                   MaterialDiagnosticCode::Cycle, node.id, pin, "Data flow cycle");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    if (state != nullptr && *state == NodeState::Done) {
        // 오류 없이 번역한 노드에 그 핀 값이 없으면 lowering이 내지 않은 출력 핀이다.
        if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                   MaterialDiagnosticCode::InvalidProperty, node.id, pin,
                                   "Output pin has no value");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }
    }

    return Result<IrValueId>::Success(NoIrValue);
}

Result<void> Translator::LowerNodeOnce(const nodegraph::GraphNode& node) noexcept {
    if (auto marked = states_.InsertOrAssign(node.id, NodeState::Visiting); !marked) {
        return Result<void>::Failure(marked.Error());
    }

    const uint32 errorsBefore = errorCount_;
    // 연결된 입력이 값 없이 들어오면(위쪽 오류) 이 노드도 실패다 — 받는 쪽이 다시 진단하지 않는다.
    bool poisoned = false;
    Result<void> lowered = Result<void>::Success();
    if (!(rerouteType_ == Name()) && node.type == rerouteType_) {
        // 공통 재경유(Graph.Reroute)는 입력을 그대로 넘긴다.
        bool connected = false;
        auto value = LowerConnection(node, rerouteIn_, connected);
        if (!value) {
            lowered = Result<void>::Failure(value.Error());
        } else if (!connected) {
            lowered = Report(nodegraph::GraphDiagnosticSeverity::Error,
                             MaterialDiagnosticCode::MissingInput, node.id, rerouteIn_,
                             "Reroute has no input");
        } else if (value.Value() == NoIrValue) {
            poisoned = true;
        } else {
            lowered = RecordOutput(node.id, rerouteOut_, value.Value());
        }
    } else if (const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
               definition == nullptr) {
        lowered =
            Report(nodegraph::GraphDiagnosticSeverity::Error, MaterialDiagnosticCode::UnknownNode,
                   node.id, Name(), "Node type is not registered");
    } else if (LoweringFn fn = FindLowering(node.type); fn == nullptr) {
        SystemAllocator scratch;
        String message(scratch);
        const StringView prerequisite = MaterialPrerequisiteId(definition->prerequisite);
        const bool waiting = definition->status == MaterialNodeStatus::PrerequisiteMissing &&
                             !prerequisite.IsEmpty();
        Result<void> built = waiting
                                 ? message.TryAssign("Node is not translated until prerequisite ")
                                 : message.TryAssign("Node is not translated in this stage yet");
        if (built && waiting) {
            built = message.TryAppend(prerequisite);
            if (built) {
                built = message.TryAppend(" exists");
            }
        }

        lowered =
            built ? Report(nodegraph::GraphDiagnosticSeverity::Error,
                           MaterialDiagnosticCode::NotYetLowered, node.id, Name(), message.View())
                  : Move(built);
    } else {
        LoweringContext context(*this, node, *definition);
        lowered = fn(context);
        poisoned = context.Poisoned();
    }

    if (!lowered) {
        return lowered;
    }

    const NodeState state =
        errorCount_ == errorsBefore && !poisoned ? NodeState::Done : NodeState::Failed;
    if (auto marked = states_.InsertOrAssign(node.id, state); !marked) {
        return Result<void>::Failure(marked.Error());
    }

    return Result<void>::Success();
}

Result<void> Translator::CollectDependencies(usize node, Array<Dependency>& out) noexcept {
    out.Clear();
    const nodegraph::GraphNode& consumer = graph_->nodes[node];
    const MaterialNodeDefinition* consumerDefinition =
        consumer.enableState == nodegraph::NodeEnableState::Disabled
            ? input_.catalog.Find(consumer.type)
            : nullptr;
    for (usize at = incomingStart_[node]; at < incomingStart_[node + 1]; ++at) {
        const nodegraph::GraphEdge& edge = graph_->edges[incoming_[at]];
        if (consumer.enableState == nodegraph::NodeEnableState::Disabled) {
            // 비활성 노드는 통과 쌍의 입력만 넘긴다(그 밖의 위쪽은 번역하지 않는다 —
            // NodeGraph 6.5절).
            bool passes = false;
            if (consumerDefinition != nullptr) {
                for (const MaterialPassThrough& pair : consumerDefinition->passThrough) {
                    auto inputPin = input_.names.TryFind(pair.input);
                    passes = passes || (inputPin && inputPin.Value() == edge.toPin);
                }
            }

            if (!passes) {
                continue;
            }
        }

        if (const usize* source = nodes_.Find(edge.fromNode)) {
            if (auto added = out.TryPushBack(Dependency{*source, PinKey{edge.toNode, edge.toPin}});
                !added) {
                return added;
            }
        }
    }

    // 이름 있는 재경유 쓰임은 선언 노드의 출력을 읽는다(간선이 없는 의존).
    if (!(namedRerouteUsage_ == Name()) && consumer.type == namedRerouteUsage_) {
        const MaterialNodeDefinition* definition = input_.catalog.Find(consumer.type);
        if (definition != nullptr && definition->propertiesSchema != nullptr) {
            SystemAllocator scratch;
            auto reader = NodePropertyReader::Open(scratch, consumer.properties.bytes.View(),
                                                   definition->propertiesSchema());
            if (reader) {
                auto declaration = reader.Value().ReadGuid("declaration", Guid{});
                if (declaration) {
                    if (const usize* source = nodes_.Find(declaration.Value())) {
                        if (auto added =
                                out.TryPushBack(Dependency{*source, PinKey{consumer.id, Name()}});
                            !added) {
                            return added;
                        }
                    }
                }
            }
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::PlanOrder(const nodegraph::GraphNode& output, Span<const Name> roots,
                                   Array<usize>& order) noexcept {
    // 반복 깊이 우선(회색 = 방문 중, 검정 = 끝). 출력 노드의 활성 핀에서만 시작한다.
    enum class Mark : uint8 {
        White = 0,
        Gray = 1,
        Black = 2
    };

    struct Frame {
        usize node = 0;
        usize next = 0;
        usize first = 0;
        usize count = 0;
    };

    SystemAllocator scratch;
    Array<Mark> marks(scratch);
    if (auto sized = marks.TryResize(graph_->nodes.Size()); !sized) {
        return sized;
    }

    for (Mark& mark : marks) {
        mark = Mark::White;
    }

    // 프레임마다 의존 목록을 한 배열에 이어 둔다(프레임이 끝나면 그 꼬리를 버린다).
    Array<Dependency> pending(scratch);
    Array<Dependency> collected(scratch);
    Array<Frame> stack(scratch);
    const auto push = [&](usize node) noexcept -> Result<void> {
        marks[node] = Mark::Gray;
        if (auto gathered = CollectDependencies(node, collected); !gathered) {
            return gathered;
        }

        Frame frame;
        frame.node = node;
        frame.first = pending.Size();
        frame.count = collected.Size();
        for (const Dependency& dependency : collected) {
            if (auto added = pending.TryPushBack(dependency); !added) {
                return added;
            }
        }

        return stack.TryPushBack(frame);
    };

    for (const nodegraph::GraphEdge& edge : graph_->edges) {
        bool active = false;
        for (Name root : roots) {
            active = active || edge.toPin == root;
        }

        if (!(edge.toNode == output.id) || !active) {
            continue;
        }

        const usize* source = nodes_.Find(edge.fromNode);
        if (source == nullptr || marks[*source] != Mark::White) {
            continue;
        }

        if (auto pushed = push(*source); !pushed) {
            return pushed;
        }

        while (!stack.IsEmpty()) {
            Frame& top = stack[stack.Size() - 1];
            if (top.next < top.count) {
                const Dependency dependency = pending[top.first + top.next];
                ++top.next;
                if (marks[dependency.source] == Mark::Gray) {
                    // 순환 간선이다: 진단하고 그 입력을 끊는다.
                    if (auto cut = cutInputs_.Insert(dependency.input, true); !cut) {
                        return Result<void>::Failure(cut.Error());
                    }

                    if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                               MaterialDiagnosticCode::Cycle, dependency.input.node,
                                               dependency.input.pin, "Data flow cycle");
                        !reported) {
                        return reported;
                    }
                } else if (marks[dependency.source] == Mark::White) {
                    if (auto pushed = push(dependency.source); !pushed) {
                        return pushed;
                    }
                }

                continue;
            }

            const Frame finished = top;
            (void)stack.TryPopBack();
            if (auto shrunk = pending.TryResize(finished.first); !shrunk) {
                return shrunk;
            }

            marks[finished.node] = Mark::Black;
            if (auto added = order.TryPushBack(finished.node); !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::CheckParameterNames() noexcept {
    // V8(6.8절): 파라미터 표지 노드의 parameterName이 대소문자 무시로 같고 종류가 다르면 오류다.
    struct Seen {
        StringView name;
        Name type;
    };

    SystemAllocator scratch;
    Array<Seen> seen(scratch);
    for (const nodegraph::GraphNode& node : graph_->nodes) {
        const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
        if (definition == nullptr || definition->propertiesSchema == nullptr ||
            !HasNodeFlag(definition->flags, MaterialNodeFlags::Parameter)) {
            continue;
        }

        auto reader = NodePropertyReader::Open(scratch, node.properties.bytes.View(),
                                               definition->propertiesSchema());
        if (!reader) {
            // 속성을 읽지 못하는 노드는 그 노드의 lowering이 진단한다.
            continue;
        }

        auto name = reader.Value().ReadText("parameterName");
        if (!name || name.Value().IsEmpty()) {
            continue;
        }

        for (const Seen& earlier : seen) {
            if (NameTextEquals(earlier.name, name.Value()) && !(earlier.type == node.type)) {
                if (auto reported =
                        Report(nodegraph::GraphDiagnosticSeverity::Error,
                               MaterialDiagnosticCode::ParameterConflict, node.id, Name(),
                               "Parameter name is used by a parameter of another kind");
                    !reported) {
                    return reported;
                }

                break;
            }
        }

        if (auto added = seen.TryPushBack(Seen{name.Value(), node.type}); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::TranslateOutputs(const nodegraph::GraphNode& output) noexcept {
    const MaterialNodeDefinition* definition = input_.catalog.Find(output.type);
    if (definition == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto marked = states_.InsertOrAssign(output.id, NodeState::Visiting); !marked) {
        return Result<void>::Failure(marked.Error());
    }

    LoweringContext context(*this, output, *definition);
    for (StringView pin : PendingOutputPins) {
        auto connected = context.IsConnected(pin);
        if (!connected) {
            return Result<void>::Failure(connected.Error());
        }

        if (connected.Value()) {
            if (auto reported = context.Error(MaterialDiagnosticCode::NotYetLowered, pin,
                                              "Output pin is not translated in this stage yet");
                !reported) {
                return reported;
            }
        }
    }

    for (const OutputSlot& slot : OutputSlots) {
        auto connected = context.IsConnected(slot.pin);
        if (!connected) {
            return Result<void>::Failure(connected.Error());
        }

        if (!SlotInContext(slot.entry, context_)) {
            continue;
        }

        if (!IsOutputActive(slot.pin, input_.traits)) {
            if (connected.Value()) {
                if (auto reported = context.Warning(
                        MaterialDiagnosticCode::InactiveOutput, slot.pin,
                        "Output pin is not used by this domain and blend mode; it is ignored");
                    !reported) {
                    return reported;
                }
            }

            continue;
        }

        auto value = context.Input(slot.pin);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        auto coerced = context.Coerce(value.Value(), slot.type, slot.pin);
        if (!coerced) {
            return Result<void>::Failure(coerced.Error());
        }

        if (coerced.Value() == NoIrValue) {
            continue;
        }

        const IrEntry entry =
            context_ == EvaluationContext::VertexPrevious ? IrEntry::VertexPrevious : slot.entry;
        if (auto added = results_.TryPushBack(IrResult{entry, slot.pin, coerced.Value()}); !added) {
            return added;
        }
    }

    if (auto marked = states_.InsertOrAssign(output.id, NodeState::Done); !marked) {
        return Result<void>::Failure(marked.Error());
    }

    return Result<void>::Success();
}

Result<void> Translator::ContextRoots(Array<Name>& roots) const noexcept {
    roots.Clear();
    for (const OutputSlot& slot : OutputSlots) {
        if (!SlotInContext(slot.entry, context_) || !IsOutputActive(slot.pin, input_.traits)) {
            continue;
        }

        auto pin = input_.names.TryFind(slot.pin);
        if (!pin) {
            if (pin.Error() == ErrorCode::NotFound) {
                // 문서에 없는 핀 이름이면 그 핀에 연결도 없다.
                continue;
            }

            return Result<void>::Failure(pin.Error());
        }

        if (auto added = roots.TryPushBack(pin.Value()); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::CheckContextValues() noexcept {
    // 진입점 결과에서 닿는 값만 본다(DCE 뒤 — UE도 활성 속성에서 닿는 식만 컴파일한다). 단계는
    // 정점(정점·이전 정점 진입점)과 픽셀 둘이다.
    const Span<const MaterialContextValue> table = MaterialContextValues();
    SystemAllocator scratch;
    Array<bool> visited(scratch);
    if (auto sized = visited.TryResize(builder_.Count()); !sized) {
        return sized;
    }

    Array<IrValueId> stack(scratch);
    for (uint32 stage = 0; stage < 2; ++stage) {
        const bool vertex = stage == 0;
        for (bool& item : visited) {
            item = false;
        }

        for (const IrResult& result : results_) {
            if (IsVertexEntry(result.entry) != vertex) {
                continue;
            }

            if (auto pushed = stack.TryPushBack(result.value); !pushed) {
                return pushed;
            }
        }

        while (!stack.IsEmpty()) {
            auto popped = stack.TryPopBack();
            if (!popped) {
                return Result<void>::Failure(popped.Error());
            }

            const IrValueId id = popped.Value();
            if (visited[id]) {
                continue;
            }

            visited[id] = true;
            const IrValue& value = builder_.Value(id);
            if (vertex && (value.op == IrOp::Ddx || value.op == IrOp::Ddy)) {
                if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                           MaterialDiagnosticCode::StageViolation, value.site.node,
                                           Name(), "Pixel-only value reaches a vertex output");
                    !reported) {
                    return reported;
                }

                continue;
            }

            if (value.op == IrOp::Extern && value.immediate[0] < table.Size()) {
                const MaterialContextValue& context = table[static_cast<usize>(value.immediate[0])];
                const MaterialStageMask stageMask =
                    vertex ? MaterialVertexStage : MaterialPixelStage;
                if ((context.stages & stageMask) == 0) {
                    SystemAllocator text;
                    String message(text);
                    const StringView parts[] = {"'", context.name, "' is not available in the ",
                                                vertex ? StringView("vertex") : StringView("pixel"),
                                                " stage"};
                    for (StringView part : parts) {
                        if (auto appended = message.TryAppend(part); !appended) {
                            return appended;
                        }
                    }

                    if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                               MaterialDiagnosticCode::StageViolation,
                                               value.site.node, Name(), message.View());
                        !reported) {
                        return reported;
                    }
                }

                if ((context.domains & ToMask(input_.traits.domain)) == 0) {
                    SystemAllocator text;
                    String message(text);
                    const StringView parts[] = {"'", context.name, "' is not available in the ",
                                                DomainText(input_.traits.domain),
                                                " material domain"};
                    for (StringView part : parts) {
                        if (auto appended = message.TryAppend(part); !appended) {
                            return appended;
                        }
                    }

                    if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                               MaterialDiagnosticCode::UnsupportedCapability,
                                               value.site.node, Name(), message.View());
                        !reported) {
                        return reported;
                    }
                }
            }

            for (uint8 index = 0; index < value.operandCount; ++index) {
                if (auto pushed = stack.TryPushBack(value.operands[index]); !pushed) {
                    return pushed;
                }
            }
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::Run() noexcept {
    out_.module.Clear();
    out_.diagnostics.items.Clear();
    out_.succeeded = false;
    auto domain = input_.names.TryFind(MaterialDomainName);
    if (!domain || !(input_.document.domain == domain.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const nodegraph::Graph& graph : input_.document.graphs) {
        if (graph.id == input_.document.mainGraph) {
            graph_ = &graph;
        }
    }

    if (graph_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto indexed = BuildIndex(); !indexed) {
        return indexed;
    }

    auto outputType = input_.names.TryFind(MaterialOutputNodeTypeName);
    const nodegraph::GraphNode* output = nullptr;
    if (outputType) {
        for (const nodegraph::GraphNode& node : graph_->nodes) {
            if (node.type == outputType.Value()) {
                output = &node;
                break;
            }
        }
    }

    if (output == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto checked = CheckParameterNames(); !checked) {
        return checked;
    }

    // 평가 문맥마다(정점 → 이전 정점 → 픽셀 — 6.9절) 의존 후위 순서로 번역한다(입력이 늘 먼저 —
    // 재귀 없음). 비활성 노드는 번역하지 않는다(받는 쪽이 통과 쌍을 따른다). 이전 정점 문맥은 정점
    // 문맥이 WPO 결과를 냈을 때만 번역한다(UE HLSLMaterialTranslator.cpp:1663–1667).
    SystemAllocator scratch;
    Array<Name> roots(scratch);
    Array<usize> order(scratch);
    const EvaluationContext contexts[] = {
        EvaluationContext::Vertex, EvaluationContext::VertexPrevious, EvaluationContext::Pixel};
    for (EvaluationContext context : contexts) {
        if (context == EvaluationContext::VertexPrevious) {
            bool hasOffset = false;
            for (const IrResult& result : results_) {
                hasOffset = hasOffset || result.entry == IrEntry::Vertex;
            }

            if (!hasOffset) {
                continue;
            }
        }

        context_ = context;
        outputs_.Clear();
        states_.Clear();
        if (auto rooted = ContextRoots(roots); !rooted) {
            return rooted;
        }

        order.Clear();
        if (auto planned = PlanOrder(*output, roots.View(), order); !planned) {
            return planned;
        }

        for (usize index : order) {
            const nodegraph::GraphNode& node = graph_->nodes[index];
            if (node.enableState == nodegraph::NodeEnableState::Disabled ||
                states_.Contains(node.id)) {
                continue;
            }

            if (auto lowered = LowerNodeOnce(node); !lowered) {
                return lowered;
            }
        }

        if (auto translated = TranslateOutputs(*output); !translated) {
            return translated;
        }
    }

    if (auto checked = CheckContextValues(); !checked) {
        return checked;
    }

    if (failed_) {
        return Result<void>::Success();
    }

    if (auto built = BuildIrModule(builder_, results_.View(), out_.module); !built) {
        return built;
    }

    out_.succeeded = true;
    return Result<void>::Success();
}

} // namespace rocks::material::detail
