#include "Translator.h"

#include <Base/NumericLimits.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/MaterialGraphTypes.h>
#include <MaterialSchema/MaterialGuidPinKey.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallInput.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallOutput.generated.h>
#include <NodeGraph/GraphClone.h>

// 함수 호출·MaterialSample 펼치기다(Docs/Design/MaterialSystem 6.12절 MS2e2). 번역 전에 주 그래프를
// 복사하고 호출된 그래프의 노드·간선을 사본(id = SHA-256(호출 노드, 원래 id) 앞 128 bit)으로
// 붙인다. 사본 안의 호출 노드도 같은 반복이 펼치므로 호출 경로마다 사본이 따로 생긴다(UE 인라인
// compile·MIR 서브그래프 문맥과 같다). 호출 노드 자체는 번역하지 않고 간선이 그 출력 사본의
// 입력으로 넘어간다.
namespace rocks::material::detail {

namespace {

constexpr StringView FunctionCallTypeName = "Material.FunctionCall";
constexpr StringView FunctionInputTypeName = "Material.FunctionInput";
constexpr StringView FunctionOutputTypeName = "Material.FunctionOutput";
constexpr StringView MaterialSampleTypeName = "Material.MaterialSample";
// 함수 호출 입출력 동적 핀 키 접두다(19.13.3절 P_<id>).
constexpr StringView FunctionCallPinPrefix = "P_";
// FunctionOutput 값 입력·출력 노드 Surface 입력·MaterialSample 출력 핀 키다.
constexpr StringView FunctionOutputValuePin = "A";
constexpr StringView SampledSurfacePin = "Surface";
constexpr StringView MaterialSampleOutputPin = "Front Material";
// Guid 하나의 byte 수와 반쪽(uint64) byte 수, byte의 bit 수다.
constexpr usize ExpandedGuidBytes = 16;
constexpr usize ExpandedGuidHalfBytes = 8;
constexpr uint32 ExpandedByteBits = 8;
// Recursive 호출이 가리키는 snapshot이 없다(머티리얼 자신을 참조).
constexpr uint32 NoCallSnapshot = NumericLimits<uint32>::Max();
// 진단 문구의 10진 색인 버퍼 길이와 진법이다.
constexpr usize CallIndexTextBytes = 10;
constexpr uint32 CallIndexBase = 10;

// parts의 SHA-256 앞 128 bit다(사본 id·파라미터 정체 — 각 Guid는 high·low를 little endian으로).
Guid DeriveExpandedGuid(Span<const Guid> parts) noexcept {
    Sha256 hasher;
    for (const Guid& part : parts) {
        uint8 bytes[ExpandedGuidBytes];
        for (usize at = 0; at < ExpandedGuidHalfBytes; ++at) {
            bytes[at] = static_cast<uint8>(part.high >> (at * ExpandedByteBits));
            bytes[ExpandedGuidHalfBytes + at] =
                static_cast<uint8>(part.low >> (at * ExpandedByteBits));
        }

        hasher.Update(Span<const uint8>(bytes));
    }

    const Sha256Digest digest = hasher.Finish();
    Guid out;
    for (usize at = 0; at < ExpandedGuidHalfBytes; ++at) {
        out.high = (out.high << ExpandedByteBits) | digest.bytes[at];
        out.low = (out.low << ExpandedByteBits) | digest.bytes[ExpandedGuidHalfBytes + at];
    }

    return out;
}

// 구조체 배열 field 원소마다의 id다(FunctionCall inputs·outputs).
template <typename Element>
Result<void> ReadCallIds(NodePropertyReader& reader, StringView field, Array<Guid>& out) noexcept {
    auto list = reader.OpenStructArray(field, GeneratedType<Element>::Schema());
    if (!list) {
        return Result<void>::Failure(list.Error());
    }

    for (usize index = 0; index < list.Value().Count(); ++index) {
        auto element = list.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto id = element.Value().ReadGuid("id", Guid{});
        if (!id) {
            return Result<void>::Failure(id.Error());
        }

        if (auto added = out.TryPushBack(id.Value()); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

// 함수 그래프의 입출력 노드 하나다(id 속성 = 호출 핀 P_<id>).
struct FunctionBoundaryNode {
    Guid id;
    usize node = 0;
    bool input = false;
};

} // namespace

bool Translator::FunctionInputBinding(Guid node, Guid& call, Name& pin) const noexcept {
    const PinKey* binding = functionInputs_.Find(node);
    if (binding == nullptr) {
        return false;
    }

    call = binding->node;
    pin = binding->pin;
    return true;
}

Guid Translator::ParameterIdentity(Guid node, Guid parameterId, bool shared) const noexcept {
    const ExpandedOrigin* origin = origins_.Find(node);
    if (origin == nullptr) {
        return parameterId;
    }

    const Guid asset = snapshots_[origin->snapshot]->asset;
    if (shared) {
        const Guid parts[] = {asset, parameterId};
        return DeriveExpandedGuid(parts);
    }

    const Guid parts[] = {asset, parameterId, origin->call};
    return DeriveExpandedGuid(parts);
}

bool Translator::IsSampledCopy(Guid node) const noexcept {
    const ExpandedOrigin* origin = origins_.Find(node);
    return origin != nullptr && origin->sampled;
}

Result<void> Translator::ExpandFunctions() noexcept {
    const auto find = [&](StringView text) noexcept {
        auto name = input_.names.TryFind(text);
        return name ? name.Value() : Name();
    };

    functionCallType_ = find(FunctionCallTypeName);
    functionInputType_ = find(FunctionInputTypeName);
    functionOutputType_ = find(FunctionOutputTypeName);
    materialSampleType_ = find(MaterialSampleTypeName);
    const auto expandable = [&](const nodegraph::GraphNode& node) noexcept {
        return node.enableState != nodegraph::NodeEnableState::Disabled &&
               ((functionCallType_.IsValid() && node.type == functionCallType_) ||
                (materialSampleType_.IsValid() && node.type == materialSampleType_));
    };

    bool any = false;
    for (const nodegraph::GraphNode& node : graph_->nodes) {
        any = any || expandable(node);
    }

    if (!any) {
        return Result<void>::Success();
    }

    if (auto cloned = nodegraph::CloneGraph(*graph_, expanded_); !cloned) {
        return cloned;
    }

    graph_ = &expanded_;
    // 붙인 사본 안의 호출 노드도 이 반복이 만난다(호출 경로 깊이는 MaxFunctionCallDepth까지).
    for (usize index = 0; index < expanded_.nodes.Size(); ++index) {
        if (!expandable(expanded_.nodes[index])) {
            continue;
        }

        const bool sample = expanded_.nodes[index].type == materialSampleType_;
        if (auto expanded = ExpandCall(index, sample); !expanded) {
            return expanded;
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::ExpandCall(usize index, bool sample) noexcept {
    SystemAllocator scratch;
    const Guid callId = expanded_.nodes[index].id;
    CallInfo info;
    info.sample = sample;
    const auto store = [&](CallStatus status) noexcept -> Result<void> {
        info.status = status;
        auto stored = calls_.InsertOrAssign(callId, info);
        return stored ? Result<void>::Success() : Result<void>::Failure(stored.Error());
    };

    // 속성을 먼저 읽는다(사본을 붙이면 노드 배열이 옮겨진다).
    Guid asset;
    Array<Guid> inputIds(scratch);
    Array<Guid> outputIds(scratch);
    {
        const MaterialNodeDefinition* definition = input_.catalog.Find(expanded_.nodes[index].type);
        if (definition == nullptr || definition->propertiesSchema == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto reader =
            NodePropertyReader::Open(scratch, expanded_.nodes[index].properties.bytes.View(),
                                     definition->propertiesSchema());
        Result<void> read =
            reader ? Result<void>::Success() : Result<void>::Failure(reader.Error());
        if (read) {
            auto reference =
                reader.Value().OpenStruct(sample ? "materialReference" : "materialFunction",
                                          GeneratedType<MaterialAssetReference>::Schema());
            auto id = reference ? reference.Value().ReadGuid("assetId", Guid{})
                                : Result<Guid>::Failure(reference.Error());
            if (id) {
                asset = id.Value();
            } else {
                read = Result<void>::Failure(id.Error());
            }
        }

        if (read && !sample) {
            read = ReadCallIds<FunctionCallInput>(reader.Value(), "inputs", inputIds);
        }

        if (read && !sample) {
            read = ReadCallIds<FunctionCallOutput>(reader.Value(), "outputs", outputIds);
        }

        if (!read) {
            if (read.Error() == ErrorCode::OutOfMemory) {
                return read;
            }

            return store(CallStatus::Unreadable);
        }
    }

    if (asset == Guid{}) {
        return store(CallStatus::MissingAsset);
    }

    // 호출 경로(바깥으로)에 같은 에셋이 다시 나오면 순환이다(V3). 머티리얼 자신도 경로의 시작이다.
    bool parentSampled = false;
    if (const ExpandedOrigin* parent = origins_.Find(callId)) {
        parentSampled = parent->sampled;
    }

    if (asset == input_.asset) {
        info.snapshot = NoCallSnapshot;
        return store(CallStatus::Recursive);
    }

    uint32 depth = 1;
    for (const ExpandedOrigin* origin = origins_.Find(callId); origin != nullptr;
         origin = origins_.Find(origin->call)) {
        if (snapshots_[origin->snapshot]->asset == asset) {
            info.snapshot = origin->snapshot;
            return store(CallStatus::Recursive);
        }

        ++depth;
    }

    if (depth > MaxFunctionCallDepth) {
        return store(CallStatus::TooLarge);
    }

    const MaterialGraphSnapshot* snapshot = nullptr;
    if (input_.sources != nullptr) {
        snapshot =
            sample ? input_.sources->FindMaterial(asset) : input_.sources->FindFunction(asset);
    }

    const nodegraph::Graph* source = nullptr;
    if (snapshot != nullptr && snapshot->document != nullptr) {
        for (const nodegraph::Graph& graph : snapshot->document->graphs) {
            if (graph.id == snapshot->document->mainGraph) {
                source = &graph;
            }
        }
    }

    if (source == nullptr) {
        return store(CallStatus::Unavailable);
    }

    // snapshot 번호와 의존 목록이다(처음 읽은 순, 같은 (역할, 에셋)은 한 번).
    const MaterialDependencyRole role =
        sample ? MaterialDependencyRole::Material : MaterialDependencyRole::Function;
    uint32 snapshotIndex = static_cast<uint32>(snapshots_.Size());
    for (usize at = 0; at < snapshots_.Size(); ++at) {
        if (snapshots_[at] == snapshot) {
            snapshotIndex = static_cast<uint32>(at);
        }
    }

    if (snapshotIndex == snapshots_.Size()) {
        if (auto added = snapshots_.TryPushBack(snapshot); !added) {
            return added;
        }

        bool listed = false;
        for (const MaterialTranslationDependency& dependency : out_.dependencies) {
            listed = listed || (dependency.role == role && dependency.asset == asset);
        }

        if (!listed) {
            if (auto added =
                    out_.dependencies.TryPushBack(MaterialTranslationDependency{role, asset});
                !added) {
                return added;
            }
        }
    }

    info.snapshot = snapshotIndex;
    if (expanded_.nodes.Size() + source->nodes.Size() > MaxExpandedMaterialNodes ||
        expanded_.edges.Size() + source->edges.Size() > nodegraph::MaxGraphEdges) {
        return store(CallStatus::TooLarge);
    }

    // 함수 입출력 노드의 id 속성이다(호출 핀 P_<id>). MaterialSample은 출력 노드 Surface 입력이다.
    Array<FunctionBoundaryNode> boundary(scratch);
    usize sampledOutput = source->nodes.Size();
    auto outputType = input_.names.TryFind(MaterialOutputNodeTypeName);
    for (usize at = 0; at < source->nodes.Size(); ++at) {
        const nodegraph::GraphNode& node = source->nodes[at];
        if (sample) {
            if (outputType && node.type == outputType.Value() &&
                sampledOutput == source->nodes.Size()) {
                sampledOutput = at;
            }

            continue;
        }

        const bool input = functionInputType_.IsValid() && node.type == functionInputType_;
        const bool output = functionOutputType_.IsValid() && node.type == functionOutputType_;
        if (!input && !output) {
            continue;
        }

        const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
        if (definition == nullptr || definition->propertiesSchema == nullptr) {
            continue;
        }

        auto reader = NodePropertyReader::Open(scratch, node.properties.bytes.View(),
                                               definition->propertiesSchema());
        auto id =
            reader ? reader.Value().ReadGuid("id", Guid{}) : Result<Guid>::Failure(reader.Error());
        if (!id) {
            if (id.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(id.Error());
            }

            // 읽지 못하는 입출력 노드는 호출 핀에 잇지 않는다(그 노드의 lowering이 진단한다).
            continue;
        }

        if (auto added = boundary.TryPushBack(FunctionBoundaryNode{id.Value(), at, input});
            !added) {
            return added;
        }
    }

    if (sample && sampledOutput == source->nodes.Size()) {
        return store(CallStatus::Unavailable);
    }

    const auto declared = [&](Guid id, bool input) noexcept {
        for (const FunctionBoundaryNode& item : boundary) {
            if (item.id == id && item.input == input) {
                return true;
            }
        }

        return false;
    };

    for (usize at = 0; at < inputIds.Size(); ++at) {
        if (!declared(inputIds[at], true)) {
            info.index = static_cast<uint32>(at);
            return store(CallStatus::UnsetInput);
        }
    }

    for (usize at = 0; at < outputIds.Size(); ++at) {
        if (!declared(outputIds[at], false)) {
            info.index = static_cast<uint32>(at);
            return store(CallStatus::UnsetOutput);
        }
    }

    // 사본을 붙인다(source는 snapshot 문서라 expanded_가 커져도 그대로다).
    const ExpandedOrigin base{callId, Guid{}, snapshotIndex, sample || parentSampled};
    for (const nodegraph::GraphNode& original : source->nodes) {
        nodegraph::GraphNode copy(allocator_);
        if (auto cloned = nodegraph::CloneGraphNode(original, copy); !cloned) {
            return cloned;
        }

        const Guid parts[] = {callId, original.id};
        copy.id = DeriveExpandedGuid(parts);
        ExpandedOrigin origin = base;
        origin.original = original.id;
        auto recorded = origins_.Insert(copy.id, origin);
        if (!recorded) {
            return Result<void>::Failure(recorded.Error());
        }

        if (recorded.Value() == InsertOutcome::AlreadyPresent) {
            // 같은 그래프의 같은 id 노드다(문서 검증이 막는다).
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto added = expanded_.nodes.TryPushBack(Move(copy)); !added) {
            return added;
        }
    }

    for (const nodegraph::GraphEdge& original : source->edges) {
        nodegraph::GraphEdge copy = original;
        const Guid from[] = {callId, original.fromNode};
        const Guid to[] = {callId, original.toNode};
        copy.fromNode = DeriveExpandedGuid(from);
        copy.toNode = DeriveExpandedGuid(to);
        if (auto added = expanded_.edges.TryPushBack(copy); !added) {
            return added;
        }
    }

    // 호출 핀 연결이다.
    if (sample) {
        auto outputPin = input_.names.TryIntern(MaterialSampleOutputPin);
        auto surfacePin = input_.names.TryIntern(SampledSurfacePin);
        if (!outputPin || !surfacePin) {
            return Result<void>::Failure(!outputPin ? outputPin.Error() : surfacePin.Error());
        }

        const Guid parts[] = {callId, source->nodes[sampledOutput].id};
        auto bound = callOutputs_.Insert(PinKey{callId, outputPin.Value()},
                                         PinKey{DeriveExpandedGuid(parts), surfacePin.Value()});
        if (!bound) {
            return Result<void>::Failure(bound.Error());
        }

        return store(CallStatus::Expanded);
    }

    auto valuePin = input_.names.TryIntern(FunctionOutputValuePin);
    if (!valuePin) {
        return Result<void>::Failure(valuePin.Error());
    }

    for (const FunctionBoundaryNode& item : boundary) {
        auto key = MaterialGuidPinKey(input_.names, FunctionCallPinPrefix, item.id);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        const Guid parts[] = {callId, source->nodes[item.node].id};
        const Guid copyId = DeriveExpandedGuid(parts);
        // 같은 id의 입출력 노드가 둘이면 처음 것이 잇는다.
        auto bound = item.input ? functionInputs_.Insert(copyId, PinKey{callId, key.Value()})
                                : callOutputs_.Insert(PinKey{callId, key.Value()},
                                                      PinKey{copyId, valuePin.Value()});
        if (!bound) {
            return Result<void>::Failure(bound.Error());
        }
    }

    return store(CallStatus::Expanded);
}

const usize* Translator::ResolveSource(const nodegraph::GraphEdge& start) const noexcept {
    const nodegraph::GraphEdge* edge = &start;
    // 호출 사슬은 펼친 노드 수를 넘을 수 없으므로 그만큼에서 멈춘다(순환 보호).
    for (usize step = 0; step <= graph_->nodes.Size(); ++step) {
        const usize* source = nodes_.Find(edge->fromNode);
        if (source == nullptr) {
            return nullptr;
        }

        const nodegraph::GraphNode& node = graph_->nodes[*source];
        const CallInfo* call = node.enableState == nodegraph::NodeEnableState::Disabled
                                   ? nullptr
                                   : calls_.Find(node.id);
        if (call == nullptr) {
            return source;
        }

        if (call->status != CallStatus::Expanded) {
            // 호출 오류는 값을 요청할 때 진단한다(LowerCallOutput).
            return nullptr;
        }

        const PinKey* target = callOutputs_.Find(PinKey{node.id, edge->fromPin});
        const usize* next = target == nullptr ? nullptr : edges_.Find(*target);
        if (next == nullptr) {
            return nullptr;
        }

        edge = &graph_->edges[*next];
    }

    return nullptr;
}

Result<IrValueId> Translator::LowerCallOutput(const nodegraph::GraphNode& node, Name pin,
                                              bool& connected) noexcept {
    connected = true;
    const CallInfo* call = calls_.Find(node.id);
    if (call == nullptr) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (call->status != CallStatus::Expanded) {
        if (auto reported = ReportCallError(node); !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    const PinKey* target = callOutputs_.Find(PinKey{node.id, pin});
    if (target == nullptr) {
        if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                   MaterialDiagnosticCode::InvalidProperty, node.id, pin,
                                   "Output pin has no value");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    const nodegraph::GraphNode* output = FindNode(target->node);
    if (output == nullptr) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (!HasConnection(*output, target->pin)) {
        // UE MaterialFunctionCall::Compile "Missing function output connection"(ME:15774–15824).
        // MaterialSample의 참조 머티리얼 Surface가 비면 UE는 값 없이 끝난다(Rocks는 진단한다).
        SystemAllocator scratch;
        String message(scratch);
        Result<void> built = Result<void>::Success();
        if (call->sample) {
            built = message.TryAssign("Referenced material has no Surface connection");
        } else {
            StringView outputName;
            const MaterialNodeDefinition* definition = input_.catalog.Find(output->type);
            if (definition != nullptr && definition->propertiesSchema != nullptr) {
                auto opened = NodePropertyReader::Open(scratch, output->properties.bytes.View(),
                                                       definition->propertiesSchema());
                auto text = opened ? opened.Value().ReadText("outputName")
                                   : Result<StringView>::Failure(opened.Error());
                outputName = text ? text.Value() : StringView();
            }

            const StringView parts[] = {"Missing function output connection '", outputName, "'"};
            for (StringView part : parts) {
                if (built) {
                    built = message.TryAppend(part);
                }
            }
        }

        if (!built) {
            return Result<IrValueId>::Failure(built.Error());
        }

        if (auto reported =
                Report(nodegraph::GraphDiagnosticSeverity::Error,
                       MaterialDiagnosticCode::MissingInput, node.id, Name(), message.View());
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    bool inner = false;
    return LowerConnection(*output, target->pin, inner);
}

Result<void> Translator::ReportCallError(const nodegraph::GraphNode& node) noexcept {
    const CallInfo* call = calls_.Find(node.id);
    if (call == nullptr || call->status == CallStatus::Expanded) {
        return Result<void>::Success();
    }

    SystemAllocator scratch;
    String message(scratch);
    MaterialDiagnosticCode code = MaterialDiagnosticCode::MissingInput;
    const StringView name = call->snapshot < snapshots_.Size() &&
                                    call->status != CallStatus::MissingAsset &&
                                    call->status != CallStatus::Unavailable
                                ? snapshots_[call->snapshot]->name
                                : StringView();
    Result<void> built = Result<void>::Success();
    switch (call->status) {
    case CallStatus::Expanded:
        break;
    case CallStatus::Unreadable:
        code = MaterialDiagnosticCode::InvalidProperty;
        built = message.TryAssign("Node properties cannot be read");
        break;
    case CallStatus::MissingAsset:
        // UE는 MaterialSample 참조가 비면 값 없이 끝난다(Rocks는 진단한다).
        built = message.TryAssign(call->sample ? "Material Sample has no material reference"
                                               : "Missing Material Function");
        break;
    case CallStatus::Unavailable:
        built = message.TryAssign(call->sample ? "Material asset is not available"
                                               : "Material function asset is not available");
        break;
    case CallStatus::Recursive:
        code = MaterialDiagnosticCode::FunctionCycle;
        if (call->sample) {
            // UE MaterialExpressionMaterialSample.cpp:54 문구 그대로다.
            built = message.TryAssign(
                "Material Sample cannot reference this material or one of it's child instances.");
        } else {
            built = message.TryAssign("Material function call is recursive: ");
            if (built) {
                built = message.TryAppend(name);
            }
        }

        break;
    case CallStatus::TooLarge:
        code = MaterialDiagnosticCode::BudgetExceeded;
        built = message.TryAssign("Material function expansion is too large");
        break;
    case CallStatus::UnsetInput:
    case CallStatus::UnsetOutput: {
        // UE MaterialFunctionCall::Compile 문구다(ME:15774–15824).
        code = MaterialDiagnosticCode::InvalidProperty;
        char digits[CallIndexTextBytes];
        usize length = 0;
        uint32 value = call->index;
        do {
            digits[length++] = static_cast<char>('0' + value % CallIndexBase);
            value /= CallIndexBase;
        } while (value != 0 && length < CallIndexTextBytes);

        char index[CallIndexTextBytes];
        for (usize at = 0; at < length; ++at) {
            index[at] = digits[length - 1 - at];
        }

        const StringView parts[] = {"Function (", name,
                                    call->status == CallStatus::UnsetInput
                                        ? ") call input with index "
                                        : ") call output with index ",
                                    StringView(index, length), " is unset."};
        for (StringView part : parts) {
            if (built) {
                built = message.TryAppend(part);
            }
        }

        break;
    }
    }

    if (!built) {
        return built;
    }

    return Report(nodegraph::GraphDiagnosticSeverity::Error, code, node.id, Name(), message.View());
}

} // namespace rocks::material::detail
