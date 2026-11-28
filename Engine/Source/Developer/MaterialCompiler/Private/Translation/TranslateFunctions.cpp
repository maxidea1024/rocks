#include "Translator.h"

#include "DiagnosticText.h"
#include "ExpandedGuid.h"
#include "FunctionNodeNames.h"

#include <Base/NumericLimits.h>
#include <Core/Hash/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/MaterialGraphTypes.h>
#include <MaterialSchema/MaterialGuidPinKey.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallInput.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallOutput.generated.h>
#include <MaterialSchema/MaterialPinType.h>
#include <NodeGraph/GraphClone.h>
#include <NodeGraph/NodeTypeDescriptor.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 함수 호출·MaterialSample 펼치기다(Docs/Design/MaterialSystem 6.12절 MS2e2). 번역 전에 주 그래프를
// 복사하고 호출된 그래프의 노드·간선을 사본(id = SHA-256(호출 노드, 원래 id) 앞 128 bit)으로
// 붙인다. 사본 안의 호출 노드도 같은 반복이 펼치므로 호출 경로마다 사본이 따로 생긴다(UE 인라인
// compile·MIR 서브그래프 문맥과 같다). 호출 노드 자체는 번역하지 않고 간선이 그 출력 사본의
// 입력으로 넘어간다.
namespace rocks::material::detail {

namespace {

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

// 정렬 순(sortPriority 오름차순, 같으면 그래프 노드 순)으로 고친다(안정 삽입 정렬 — 목록은 작다).
void SortSignaturePins(Array<FunctionSignaturePin>& pins) noexcept {
    for (usize at = 1; at < pins.Size(); ++at) {
        for (usize back = at; back > 0 && pins[back].sortPriority < pins[back - 1].sortPriority;
             --back) {
            FunctionSignaturePin swapped = pins[back];
            pins[back] = pins[back - 1];
            pins[back - 1] = swapped;
        }
    }
}

} // namespace

// 펼친 그래프 id hash다(ExpandedGuid.h).
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

Guid Translator::CopiedNodeId(Guid consumer, Guid original) const noexcept {
    const ExpandedOrigin* origin = origins_.Find(consumer);
    if (origin == nullptr) {
        return original;
    }

    const Guid parts[] = {origin->call, original};
    return DeriveExpandedGuid(parts);
}

bool Translator::ClimbOrigin(Guid& at, const ExpandedOrigin*& origin) const noexcept {
    origin = origins_.Find(at);
    if (origin != nullptr) {
        at = origin->call;
        return true;
    }

    if (const Guid* owner = syntheticOwners_.Find(at)) {
        at = *owner;
        return true;
    }

    return false;
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
    layerOutputType_ = find(LayerOutputTypeName);
    attributeLayersType_ = find(AttributeLayersTypeName);
    layerStackType_ = find(LayerStackTypeName);
    const auto is = [](Name type, Name expected) noexcept {
        return expected.IsValid() && type == expected;
    };

    const auto expandable = [&](const nodegraph::GraphNode& node) noexcept {
        return node.enableState != nodegraph::NodeEnableState::Disabled &&
               (is(node.type, functionCallType_) || is(node.type, materialSampleType_) ||
                is(node.type, attributeLayersType_) || is(node.type, layerStackType_));
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
    // 레이어 합성 호출은 붙일 때 펼쳤으므로 건너뛴다.
    bool layerNodeSeen = false;
    for (usize index = 0; index < expanded_.nodes.Size(); ++index) {
        const nodegraph::GraphNode& node = expanded_.nodes[index];
        if (!expandable(node) || calls_.Contains(node.id)) {
            continue;
        }

        const bool layered = is(node.type, attributeLayersType_) || is(node.type, layerStackType_);
        if (layered && !origins_.Contains(node.id)) {
            // 머티리얼의 레이어 노드는 하나다(UE HLSLMaterialTranslator.cpp:1453–1466 — 번역 시작
            // 때, 쓰임과 무관).
            if (layerNodeSeen) {
                if (auto reported =
                        ReportAt(nodegraph::GraphDiagnosticSeverity::Error,
                                 MaterialDiagnosticCode::LayerStackRule, node.id, Name(),
                                 "Materials can contain only one Material Attribute Layers node.");
                    !reported) {
                    return reported;
                }
            }

            layerNodeSeen = true;
        }

        Result<void> expanded = Result<void>::Success();
        if (layered) {
            expanded = ExpandLayers(index, is(node.type, layerStackType_));
        } else {
            expanded = ExpandCall(index, is(node.type, materialSampleType_));
        }

        if (!expanded) {
            return expanded;
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::ExpandCall(usize index, bool sample) noexcept {
    SystemAllocator scratch;
    const Guid callId = expanded_.nodes[index].id;
    // 속성을 먼저 읽는다(사본을 붙이면 노드 배열이 옮겨진다).
    Guid asset;
    Array<Guid> inputIds(scratch);
    Array<Guid> outputIds(scratch);
    const MaterialNodeDefinition* definition = input_.catalog.Find(expanded_.nodes[index].type);
    if (definition == nullptr || definition->propertiesSchema == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto reader = NodePropertyReader::Open(scratch, expanded_.nodes[index].properties.bytes.View(),
                                           definition->propertiesSchema());
    Result<void> read = reader ? Result<void>::Success() : Result<void>::Failure(reader.Error());
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

        CallInfo info;
        info.sample = sample;
        info.status = CallStatus::Unreadable;
        auto stored = calls_.InsertOrAssign(callId, info);
        return stored ? Result<void>::Success() : Result<void>::Failure(stored.Error());
    }

    const ExpandedOrigin* parent = origins_.Find(callId);
    return ExpandAsset(callId, asset, sample, parent != nullptr && parent->sampled, inputIds.View(),
                       outputIds.View(), nullptr);
}

Result<void> Translator::AddDependency(MaterialDependencyRole role, Guid asset) noexcept {
    for (const MaterialTranslationDependency& dependency : out_.dependencies) {
        if (dependency.role == role && dependency.asset == asset) {
            return Result<void>::Success();
        }
    }

    return out_.dependencies.TryPushBack(MaterialTranslationDependency{role, asset});
}

Result<const MaterialGraphSnapshot*> Translator::LoadSnapshot(Guid asset, bool sample,
                                                              const nodegraph::Graph*& graph,
                                                              uint32& index) noexcept {
    graph = nullptr;
    index = 0;
    const MaterialGraphSnapshot* snapshot = nullptr;
    if (input_.sources != nullptr && !(asset == Guid{})) {
        snapshot =
            sample ? input_.sources->FindMaterial(asset) : input_.sources->FindFunction(asset);
    }

    if (snapshot != nullptr && snapshot->document != nullptr) {
        for (const nodegraph::Graph& candidate : snapshot->document->graphs) {
            if (candidate.id == snapshot->document->mainGraph) {
                graph = &candidate;
            }
        }
    }

    if (graph == nullptr) {
        return Result<const MaterialGraphSnapshot*>::Success(nullptr);
    }

    // snapshot 번호와 의존 목록이다(처음 읽은 순, 같은 (역할, 에셋)은 한 번).
    const MaterialDependencyRole role =
        sample ? MaterialDependencyRole::Material : MaterialDependencyRole::Function;
    index = static_cast<uint32>(snapshots_.Size());
    for (usize at = 0; at < snapshots_.Size(); ++at) {
        if (snapshots_[at] == snapshot) {
            index = static_cast<uint32>(at);
        }
    }

    if (index == snapshots_.Size()) {
        if (auto added = snapshots_.TryPushBack(snapshot); !added) {
            return Result<const MaterialGraphSnapshot*>::Failure(added.Error());
        }

        if (auto added = AddDependency(role, asset); !added) {
            return Result<const MaterialGraphSnapshot*>::Failure(added.Error());
        }
    }

    return Result<const MaterialGraphSnapshot*>::Success(snapshot);
}

Result<void> Translator::ReadSignature(const nodegraph::GraphDocument* document,
                                       const nodegraph::Graph& source, FunctionSignature& out,
                                       uint32 depth, bool classifyOutputs) noexcept {
    out.inputs.Clear();
    out.outputs.Clear();
    out.hasStatic = false;
    SystemAllocator scratch;
    constexpr usize StaticNodeTypeCount =
        sizeof(StaticNodeTypeNames) / sizeof(StaticNodeTypeNames[0]);
    Name statics[StaticNodeTypeCount];
    for (usize at = 0; at < StaticNodeTypeCount; ++at) {
        auto name = input_.names.TryFind(StaticNodeTypeNames[at]);
        statics[at] = name ? name.Value() : Name();
    }

    auto valuePin = input_.names.TryIntern(FunctionOutputValuePin);
    if (!valuePin) {
        return Result<void>::Failure(valuePin.Error());
    }

    for (usize at = 0; at < source.nodes.Size(); ++at) {
        const nodegraph::GraphNode& node = source.nodes[at];
        for (Name type : statics) {
            out.hasStatic = out.hasStatic || (type.IsValid() && node.type == type);
        }

        const bool input = functionInputType_.IsValid() && node.type == functionInputType_;
        const bool layerOutput = layerOutputType_.IsValid() && node.type == layerOutputType_;
        const bool output =
            layerOutput || (functionOutputType_.IsValid() && node.type == functionOutputType_);
        if (!input && !output) {
            continue;
        }

        const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
        if (definition == nullptr || definition->propertiesSchema == nullptr) {
            continue;
        }

        auto reader = NodePropertyReader::Open(scratch, node.properties.bytes.View(),
                                               definition->propertiesSchema());
        if (!reader) {
            if (reader.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(reader.Error());
            }

            // 읽지 못하는 입출력 노드는 호출 핀에 잇지 않는다(그 노드의 lowering이 진단한다).
            continue;
        }

        NodePropertyReader& properties = reader.Value();
        auto id = properties.ReadGuid("id", Guid{});
        auto priority = properties.ReadInteger("sortPriority", 0);
        auto name = properties.ReadText(input ? "inputName" : "outputName");
        if (!id || !priority || !name) {
            const ErrorCode error = !id ? id.Error() : !priority ? priority.Error() : name.Error();
            if (error == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(error);
            }

            continue;
        }

        FunctionSignaturePin pin;
        pin.id = id.Value();
        pin.node = at;
        pin.sortPriority = static_cast<int32>(priority.Value());
        pin.name = name.Value();
        auto key = MaterialGuidPinKey(input_.names, FunctionCallPinPrefix, pin.id);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        pin.key = key.Value();
        if (input) {
            auto kind =
                properties.ReadInteger("inputType", static_cast<int64>(FunctionInputType::Vector3));
            auto relevance = properties.ReadInteger(
                "blendInputRelevance", static_cast<int64>(BlendInputRelevance::General));
            if (!kind || !relevance) {
                const ErrorCode error = !kind ? kind.Error() : relevance.Error();
                if (error == ErrorCode::OutOfMemory) {
                    return Result<void>::Failure(error);
                }

                continue;
            }

            pin.type = static_cast<FunctionInputType>(kind.Value());
            pin.relevance = static_cast<BlendInputRelevance>(relevance.Value());
            pin.attributes = pin.type == FunctionInputType::MaterialAttributes;
        } else {
            // MaterialAttributes 출력 판정이다(6.12절 MS2e3).
            if (layerOutput) {
                pin.attributes = true;
            } else if (classifyOutputs) {
                auto attributes =
                    OutputIsAttributes(document, source, node.id, valuePin.Value(), depth);
                if (!attributes) {
                    return Result<void>::Failure(attributes.Error());
                }

                pin.attributes = attributes.Value();
            }
        }

        if (auto added = input ? out.inputs.TryPushBack(pin) : out.outputs.TryPushBack(pin);
            !added) {
            return added;
        }
    }

    SortSignaturePins(out.inputs);
    SortSignaturePins(out.outputs);
    return Result<void>::Success();
}

Result<bool> Translator::OutputIsAttributes(const nodegraph::GraphDocument* document,
                                            const nodegraph::Graph& source, Guid node, Name pin,
                                            uint32 depth) noexcept {
    SystemAllocator scratch;
    const auto find = [&](StringView text) noexcept {
        auto name = input_.names.TryFind(text);
        return name ? name.Value() : Name();
    };

    const Name reroute = find(nodegraph::RerouteNodeTypeName);
    const Name rerouteIn = find(nodegraph::ReroutePinIn);
    const Name namedUsage = find(NamedRerouteUsageTypeName);
    const Name namedDeclaration = find(NamedRerouteDeclarationTypeName);
    const Name declarationInput = find(NamedRerouteDeclarationInputPin);
    Guid current = node;
    Name currentPin = pin;
    // 재경유·이름 있는 재경유는 따라간다(깊이 상한 MaxLoweringDepth — 순환 보호).
    for (uint32 step = 0; step < MaxLoweringDepth; ++step) {
        const nodegraph::GraphEdge* edge = nullptr;
        for (const nodegraph::GraphEdge& candidate : source.edges) {
            if (candidate.toNode == current && candidate.toPin == currentPin) {
                edge = &candidate;
                break;
            }
        }

        const nodegraph::GraphNode* from = nullptr;
        for (usize at = 0; edge != nullptr && at < source.nodes.Size(); ++at) {
            if (source.nodes[at].id == edge->fromNode) {
                from = &source.nodes[at];
            }
        }

        if (from == nullptr) {
            return Result<bool>::Success(false);
        }

        const auto is = [](Name type, Name expected) noexcept {
            return expected.IsValid() && type == expected;
        };

        if (is(from->type, reroute)) {
            current = from->id;
            currentPin = rerouteIn;
            continue;
        }

        if (is(from->type, namedDeclaration)) {
            current = from->id;
            currentPin = declarationInput;
            continue;
        }

        const MaterialNodeDefinition* definition = input_.catalog.Find(from->type);
        if (definition == nullptr) {
            return Result<bool>::Success(false);
        }

        Optional<NodePropertyReader> properties;
        if (definition->propertiesSchema != nullptr) {
            auto opened = NodePropertyReader::Open(scratch, from->properties.bytes.View(),
                                                   definition->propertiesSchema());
            if (!opened) {
                return opened.Error() == ErrorCode::OutOfMemory
                           ? Result<bool>::Failure(opened.Error())
                           : Result<bool>::Success(false);
            }

            properties.Emplace(Move(opened.Value()));
        }

        if (is(from->type, namedUsage) && properties.HasValue()) {
            auto declaration = properties->ReadGuid("declaration", Guid{});
            if (!declaration) {
                return Result<bool>::Success(false);
            }

            current = declaration.Value();
            // 선언 노드의 출력을 읽는 것과 같다(선언 노드의 Input을 따라간다).
            for (const nodegraph::GraphNode& candidate : source.nodes) {
                if (candidate.id == current) {
                    currentPin = declarationInput;
                }
            }

            continue;
        }

        if (is(from->type, functionInputType_) && properties.HasValue()) {
            auto kind = properties->ReadInteger("inputType",
                                                static_cast<int64>(FunctionInputType::Vector3));
            return Result<bool>::Success(
                kind && kind.Value() == static_cast<int64>(FunctionInputType::MaterialAttributes));
        }

        if (is(from->type, functionCallType_) && properties.HasValue()) {
            // 중첩 호출은 그 함수 출력의 같은 판정이다(호출 깊이 상한까지).
            if (depth + 1 >= MaxFunctionCallDepth) {
                return Result<bool>::Success(false);
            }

            auto reference = properties->OpenStruct(
                "materialFunction", GeneratedType<MaterialAssetReference>::Schema());
            auto asset = reference ? reference.Value().ReadGuid("assetId", Guid{})
                                   : Result<Guid>::Failure(reference.Error());
            if (!asset) {
                return Result<bool>::Success(false);
            }

            const nodegraph::Graph* callee = nullptr;
            uint32 calleeIndex = 0;
            auto snapshot = LoadSnapshot(asset.Value(), false, callee, calleeIndex);
            if (!snapshot) {
                return Result<bool>::Failure(snapshot.Error());
            }

            if (snapshot.Value() == nullptr) {
                return Result<bool>::Success(false);
            }

            FunctionSignature signature(scratch);
            if (auto read =
                    ReadSignature(snapshot.Value()->document, *callee, signature, depth + 1, true);
                !read) {
                return Result<bool>::Failure(read.Error());
            }

            for (const FunctionSignaturePin& output : signature.outputs) {
                if (output.key == edge->fromPin) {
                    return Result<bool>::Success(output.attributes);
                }
            }

            return Result<bool>::Success(false);
        }

        for (const MaterialPinDefinition& definitionPin : definition->pins) {
            auto key = input_.names.TryFind(definitionPin.key);
            if (definitionPin.direction == nodegraph::PinDirection::Output && key &&
                key.Value() == edge->fromPin) {
                return Result<bool>::Success(definitionPin.type ==
                                             MaterialPinType::MaterialAttributes);
            }
        }

        // 동적 핀 출력은 그 노드의 동적 핀 함수 결과 타입이다.
        if (definition->dynamicPins != nullptr && document != nullptr) {
            nodegraph::PinList pins(scratch);
            const nodegraph::DynamicPinsInput pinsInput{*document, source, *from, input_.names};
            if (auto computed = definition->dynamicPins(pinsInput, pins); !computed) {
                return computed.Error() == ErrorCode::OutOfMemory
                           ? Result<bool>::Failure(computed.Error())
                           : Result<bool>::Success(false);
            }

            for (const nodegraph::PinDescriptor& descriptor : pins.View()) {
                if (descriptor.direction == nodegraph::PinDirection::Output &&
                    descriptor.key == edge->fromPin) {
                    return Result<bool>::Success(descriptor.type ==
                                                 ToPinTypeId(MaterialPinType::MaterialAttributes));
                }
            }
        }

        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(false);
}

Result<void> Translator::ExpandAsset(Guid callId, Guid asset, bool sample, bool parentSampled,
                                     Span<const Guid> inputIds, Span<const Guid> outputIds,
                                     FunctionSignature* signature) noexcept {
    SystemAllocator scratch;
    FunctionSignature local(scratch);
    FunctionSignature& pins = signature != nullptr ? *signature : local;
    pins.inputs.Clear();
    pins.outputs.Clear();
    CallInfo info;
    info.sample = sample;
    const auto store = [&](CallStatus status) noexcept -> Result<void> {
        info.status = status;
        auto stored = calls_.InsertOrAssign(callId, info);
        return stored ? Result<void>::Success() : Result<void>::Failure(stored.Error());
    };

    if (asset == Guid{}) {
        return store(CallStatus::MissingAsset);
    }

    // 호출 경로(바깥으로)에 같은 에셋이 다시 나오면 순환이다(V3). 머티리얼 자신도 경로의 시작이다.
    if (asset == input_.asset) {
        info.snapshot = NoCallSnapshot;
        return store(CallStatus::Recursive);
    }

    // 레이어 합성 호출도 넘어 끝까지 올라간다(레이어 함수 안의 재귀·깊이 — 6.12절 MS2e3).
    uint32 depth = 1;
    Guid at = callId;
    const ExpandedOrigin* origin = nullptr;
    for (usize step = 0;
         step <= origins_.Count() + syntheticOwners_.Count() && ClimbOrigin(at, origin); ++step) {
        if (origin == nullptr) {
            continue;
        }

        if (snapshots_[origin->snapshot]->asset == asset) {
            info.snapshot = origin->snapshot;
            return store(CallStatus::Recursive);
        }

        ++depth;
    }

    if (depth > MaxFunctionCallDepth) {
        return store(CallStatus::TooLarge);
    }

    const nodegraph::Graph* source = nullptr;
    uint32 snapshotIndex = 0;
    auto snapshot = LoadSnapshot(asset, sample, source, snapshotIndex);
    if (!snapshot) {
        return Result<void>::Failure(snapshot.Error());
    }

    if (snapshot.Value() == nullptr) {
        return store(CallStatus::Unavailable);
    }

    info.snapshot = snapshotIndex;
    if (expanded_.nodes.Size() + source->nodes.Size() > MaxExpandedMaterialNodes ||
        expanded_.edges.Size() + source->edges.Size() > nodegraph::MaxGraphEdges) {
        return store(CallStatus::TooLarge);
    }

    // MaterialSample은 참조 머티리얼 출력 노드 Surface 입력이다.
    usize sampledOutput = source->nodes.Size();
    if (sample) {
        auto outputType = input_.names.TryFind(MaterialOutputNodeTypeName);
        for (usize at = 0; at < source->nodes.Size(); ++at) {
            if (outputType && source->nodes[at].type == outputType.Value() &&
                sampledOutput == source->nodes.Size()) {
                sampledOutput = at;
            }
        }

        if (sampledOutput == source->nodes.Size()) {
            return store(CallStatus::MissingSurface);
        }
    } else if (auto read =
                   ReadSignature(snapshot.Value()->document, *source, pins, 0,
                                 snapshot.Value()->usage == MaterialFunctionUsage::Layer ||
                                     snapshot.Value()->usage == MaterialFunctionUsage::LayerBlend);
               !read) {
        return read;
    }

    const auto declared = [](Span<const FunctionSignaturePin> list, Guid id) noexcept {
        for (const FunctionSignaturePin& item : list) {
            if (item.id == id) {
                return true;
            }
        }

        return false;
    };

    for (usize at = 0; at < inputIds.Size(); ++at) {
        if (!declared(pins.inputs.View(), inputIds[at])) {
            info.index = static_cast<uint32>(at);
            return store(CallStatus::UnsetInput);
        }
    }

    for (usize at = 0; at < outputIds.Size(); ++at) {
        if (!declared(pins.outputs.View(), outputIds[at])) {
            info.index = static_cast<uint32>(at);
            return store(CallStatus::UnsetOutput);
        }
    }

    // 함수 용도 검사다(UE UMaterialFunction::ValidateFunctionUsage — 6.12절 MS2e3).
    if (!sample) {
        const MaterialFunctionUsage usage = snapshot.Value()->usage;
        info.attributeInputs = static_cast<uint32>(pins.AttributeInputs());
        info.attributeOutputs = static_cast<uint32>(pins.AttributeOutputs());
        const bool outputsValid = info.attributeOutputs == AcceptableLayerBlendAttributeOutputs;
        if ((usage == MaterialFunctionUsage::Layer &&
             (!outputsValid || info.attributeInputs > AcceptableLayerAttributeInputs)) ||
            (usage == MaterialFunctionUsage::LayerBlend &&
             (!outputsValid || info.attributeInputs != AcceptableBlendAttributeInputs))) {
            return store(CallStatus::InvalidUsage);
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

    for (const FunctionSignaturePin& item : pins.inputs) {
        const Guid parts[] = {callId, source->nodes[item.node].id};
        // 입력 노드마다 사본이 그 호출 핀에 묶인다(같은 id 둘이면 둘 다 같은 핀을 읽는다).
        if (auto bound =
                functionInputs_.Insert(DeriveExpandedGuid(parts), PinKey{callId, item.key});
            !bound) {
            return Result<void>::Failure(bound.Error());
        }
    }

    for (const FunctionSignaturePin& item : pins.outputs) {
        const Guid parts[] = {callId, source->nodes[item.node].id};
        if (auto bound = callOutputs_.Insert(PinKey{callId, item.key},
                                             PinKey{DeriveExpandedGuid(parts), valuePin.Value()});
            !bound) {
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
        // 함수에 그 출력이 없다(UE "Invalid function (%s) output" — ME:15797–15800, 호출 쪽 경로).
        SystemAllocator scratch;
        String message(scratch);
        const StringView name =
            call->snapshot < snapshots_.Size() ? snapshots_[call->snapshot]->name : StringView();
        const DiagnosticTextPart parts[] = {DiagnosticWord("Invalid function ("),
                                            DiagnosticWord(name), DiagnosticWord(") output")};
        Result<void> built = call->sample ? message.TryAssign("Output pin has no value")
                                          : AppendDiagnosticText(message, parts);
        if (!built) {
            return Result<IrValueId>::Failure(built.Error());
        }

        if (auto reported =
                Report(nodegraph::GraphDiagnosticSeverity::Error,
                       MaterialDiagnosticCode::InvalidProperty, node.id, Name(), message.View());
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

        // 함수 출력 연결 없음은 UE가 PushFunction 뒤에 낸다(ME:14267 — 경로에 그 함수 이름까지).
        if (auto reported = ReportInFunction(
                nodegraph::GraphDiagnosticSeverity::Error, MaterialDiagnosticCode::MissingInput,
                node.id, Name(), message.View(), call->sample ? NoInnerFunction : call->snapshot);
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
    if (call->status == CallStatus::InvalidUsage) {
        return ReportUsageError(node);
    }

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
    case CallStatus::InvalidUsage:
        break;
    case CallStatus::MissingSurface:
        built = message.TryAssign("Referenced material has no Surface connection");
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
        const DiagnosticTextPart parts[] = {
            DiagnosticWord("Function ("), DiagnosticWord(name),
            DiagnosticWord(call->status == CallStatus::UnsetInput ? ") call input with index "
                                                                  : ") call output with index "),
            DiagnosticNumber(call->index), DiagnosticWord(" is unset.")};
        built = AppendDiagnosticText(message, parts);
        break;
    }
    }

    if (!built) {
        return built;
    }

    return Report(nodegraph::GraphDiagnosticSeverity::Error, code, node.id, Name(), message.View());
}

} // namespace rocks::material::detail
