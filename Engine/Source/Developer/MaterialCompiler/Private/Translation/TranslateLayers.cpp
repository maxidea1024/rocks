#include "Translator.h"

#include "DiagnosticText.h"
#include "ExpandedGuid.h"
#include "FunctionNodeNames.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackDefaults.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackInput.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.generated.h>
#include <NodeGraph/NodeTypeDescriptor.h>

// 레이어 노드(AttributeLayers·LayerStack) 펼치기다(Docs/Design/MaterialSystem 6.12절 MS2e3 — UE
// MaterialExpressions.cpp:7206–7600, MaterialExpressionLayerStack.cpp:294–720). 활성
// 레이어·혼합마다 합성 함수 호출을 붙이고(펼치기는 보통 호출과 같다) 입력을 합성 간선으로 잇는다.
// 레이어 노드 lowering은 사슬 끝 합성 호출의 첫 출력을 읽는다.
namespace rocks::material::detail {

namespace {

// 합성 호출 id의 역할 값이다(Hash(레이어 노드, layerId, 역할)).
constexpr uint64 LayerCallRole = 1;
constexpr uint64 BlendCallRole = 2;
// 함수 역할 수다(레이어·혼합 — 목록 둘을 차례로 본다).
constexpr usize LayerFunctionRoleCount = 2;
// AttributeLayers 입력 핀 키다.
constexpr StringView LayerNodeInputPin = "Input";

// 레이어 항목 하나다(LayerStackLayer — 아래(0)부터 위 순서).
struct LayerEntry {
    Guid layerId;
    Guid layer;
    Guid blend;
    bool visible = true;
};

// LayerStack layerInputs 항목 하나다(동적 입력 키와 이름·종류).
struct LayerStackInputEntry {
    StringView name;
    FunctionInputType type = FunctionInputType::Vector3;
    Name key;
};

// 레이어 합성 호출 id다(Hash(레이어 노드, layerId, 역할) — 6.12절 MS2e3).
Guid LayerCallGuid(Guid owner, Guid layerId, uint64 role) noexcept {
    const Guid parts[] = {owner, layerId, Guid{0, role}};
    return DeriveExpandedGuid(parts);
}

// MaterialAssetReference 칸의 에셋 Guid다.
Result<Guid> ReadLayerReference(NodePropertyReader& reader, StringView field) noexcept {
    auto reference = reader.OpenStruct(field, GeneratedType<MaterialAssetReference>::Schema());
    if (!reference) {
        return Result<Guid>::Failure(reference.Error());
    }

    return reference.Value().ReadGuid("assetId", Guid{});
}

// 블렌드 함수의 아래·위 입력이다. relevance Bottom·Top인 입력을 먼저 쓰고, attributesOnly가
// 아니면 없을 때 정렬 순 0·1이다(6.12절 MS2e3).
void BlendInputs(const FunctionSignature& signature, bool attributesOnly,
                 const FunctionSignaturePin*& bottom, const FunctionSignaturePin*& top) noexcept {
    bottom = nullptr;
    top = nullptr;
    for (const FunctionSignaturePin& pin : signature.inputs) {
        if (attributesOnly && !pin.attributes) {
            continue;
        }

        if (bottom == nullptr && pin.relevance == BlendInputRelevance::Bottom) {
            bottom = &pin;
        }

        if (top == nullptr && pin.relevance == BlendInputRelevance::Top) {
            top = &pin;
        }
    }

    if (attributesOnly) {
        return;
    }

    // relevance가 없는 쪽은 아직 고르지 않은 입력 중 정렬 순 첫째다(아래 먼저 — 같은 입력을 두 번
    // 고르지 않는다).
    for (const FunctionSignaturePin& pin : signature.inputs) {
        if (bottom == nullptr && &pin != top) {
            bottom = &pin;
        }
    }

    for (const FunctionSignaturePin& pin : signature.inputs) {
        if (top == nullptr && &pin != bottom) {
            top = &pin;
        }
    }
}

} // namespace

Result<void> Translator::AddExpandedEdge(Guid fromNode, Name fromPin, Guid toNode,
                                         Name toPin) noexcept {
    nodegraph::GraphEdge edge;
    edge.fromNode = fromNode;
    edge.fromPin = fromPin;
    edge.toNode = toNode;
    edge.toPin = toPin;
    // 입력 하나에 간선은 하나다. 나중 간선(사슬)이 앞 간선(layerInputs)을 덮는다(UE
    // MaterialExpressionLayerStack.cpp:541).
    for (nodegraph::GraphEdge& existing : expanded_.edges) {
        if (existing.toNode == toNode && existing.toPin == toPin) {
            existing = edge;
            return Result<void>::Success();
        }
    }

    return expanded_.edges.TryPushBack(edge);
}

Result<Guid> Translator::AddLayerCall(Guid owner, Guid layerId, uint32 role, Guid asset,
                                      bool parentSampled, FunctionSignature& signature) noexcept {
    if (!functionCallType_.IsValid()) {
        auto interned = input_.names.TryIntern(FunctionCallTypeName);
        if (!interned) {
            return Result<Guid>::Failure(interned.Error());
        }

        functionCallType_ = interned.Value();
    }

    const Guid id = LayerCallGuid(owner, layerId, role);
    nodegraph::GraphNode node(allocator_);
    node.id = id;
    node.type = functionCallType_;
    if (auto added = expanded_.nodes.TryPushBack(Move(node)); !added) {
        return Result<Guid>::Failure(added.Error());
    }

    if (auto recorded = syntheticOwners_.Insert(id, owner); !recorded) {
        return Result<Guid>::Failure(recorded.Error());
    }

    if (auto expanded = ExpandAsset(id, asset, false, parentSampled, {}, {}, &signature);
        !expanded) {
        return Result<Guid>::Failure(expanded.Error());
    }

    return Result<Guid>::Success(id);
}

Result<void> Translator::ExpandLayers(usize index, bool stack) noexcept {
    SystemAllocator scratch;
    const Guid owner = expanded_.nodes[index].id;
    LayerResult result;
    result.firstDiagnostic = static_cast<uint32>(layerDiagnostics_.Size());
    const auto diagnose = [&](Span<const DiagnosticTextPart> parts, bool prefixed) noexcept {
        String text(allocator_);
        Result<void> built =
            prefixed ? text.TryAssign(LayerValidationPrefix) : Result<void>::Success();
        if (built) {
            built = AppendDiagnosticText(text, parts);
        }

        if (!built) {
            return built;
        }

        return layerDiagnostics_.TryPushBack(Move(text));
    };

    const auto finish = [&]() noexcept -> Result<void> {
        if (layerDiagnostics_.Size() > result.firstDiagnostic) {
            // UE UMaterialExpressionMaterialAttributeLayers::Compile의 마지막 문구다(접두 없음).
            const DiagnosticTextPart failed[] = {
                DiagnosticWord("Failed to validate layer configuration.")};
            if (auto added = diagnose(failed, false); !added) {
                return added;
            }

            result.valid = false;
        }

        result.diagnosticCount =
            static_cast<uint32>(layerDiagnostics_.Size()) - result.firstDiagnostic;
        auto stored = layers_.InsertOrAssign(owner, result);
        return stored ? Result<void>::Success() : Result<void>::Failure(stored.Error());
    };

    // 속성을 먼저 읽는다(합성 호출을 붙이면 노드 배열이 옮겨진다).
    Array<LayerEntry> entries(scratch);
    Array<Guid> availableLayers(scratch);
    Array<Guid> availableBlends(scratch);
    Array<LayerStackInputEntry> layerInputs(scratch);
    Optional<nodegraph::GraphEdge> inputEdge;
    {
        const nodegraph::GraphNode& node = expanded_.nodes[index];
        const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
        if (definition == nullptr || definition->propertiesSchema == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto reader = NodePropertyReader::Open(scratch, node.properties.bytes.View(),
                                               definition->propertiesSchema());
        Result<void> read =
            reader ? Result<void>::Success() : Result<void>::Failure(reader.Error());
        if (read) {
            auto defaults = reader.Value().OpenStruct("defaultLayers",
                                                      GeneratedType<LayerStackDefaults>::Schema());
            auto list = defaults ? defaults.Value().OpenStructArray(
                                       "layers", GeneratedType<LayerStackLayer>::Schema())
                                 : Result<NodePropertyList>::Failure(defaults.Error());
            read = list ? Result<void>::Success() : Result<void>::Failure(list.Error());
            for (usize at = 0; read && at < list.Value().Count(); ++at) {
                auto element = list.Value().Element(at);
                if (!element) {
                    read = Result<void>::Failure(element.Error());
                    break;
                }

                auto layerId = element.Value().ReadGuid("layerId", Guid{});
                auto layer = ReadLayerReference(element.Value(), "layer");
                auto blend = ReadLayerReference(element.Value(), "blend");
                auto visible = element.Value().ReadBool("visible", true);
                if (!layerId || !layer || !blend || !visible) {
                    read = Result<void>::Failure(!layerId ? layerId.Error()
                                                 : !layer ? layer.Error()
                                                 : !blend ? blend.Error()
                                                          : visible.Error());
                    break;
                }

                read = entries.TryPushBack(
                    LayerEntry{layerId.Value(), layer.Value(), blend.Value(), visible.Value()});
            }
        }

        if (read && stack) {
            const StringView lists[] = {"availableLayers", "availableBlends"};
            Array<Guid>* targets[] = {&availableLayers, &availableBlends};
            for (usize which = 0; read && which < sizeof(lists) / sizeof(lists[0]); ++which) {
                auto list = reader.Value().OpenStructArray(
                    lists[which], GeneratedType<MaterialAssetReference>::Schema());
                read = list ? Result<void>::Success() : Result<void>::Failure(list.Error());
                for (usize at = 0; read && at < list.Value().Count(); ++at) {
                    auto element = list.Value().Element(at);
                    auto id = element ? element.Value().ReadGuid("assetId", Guid{})
                                      : Result<Guid>::Failure(element.Error());
                    read = id ? targets[which]->TryPushBack(id.Value())
                              : Result<void>::Failure(id.Error());
                }
            }

            // layerInputs의 동적 입력 키는 노드의 동적 핀 순서다(UE LayerInputs — 19.13.3절).
            nodegraph::PinList pins(scratch);
            if (read && definition->dynamicPins != nullptr) {
                const nodegraph::DynamicPinsInput pinsInput{input_.document, expanded_, node,
                                                            input_.names};
                read = definition->dynamicPins(pinsInput, pins);
            }

            auto list = read ? reader.Value().OpenStructArray(
                                   "layerInputs", GeneratedType<LayerStackInput>::Schema())
                             : Result<NodePropertyList>::Failure(ErrorCode::InvalidArgument);
            if (read) {
                read = list ? Result<void>::Success() : Result<void>::Failure(list.Error());
            }

            for (usize at = 0; read && at < list.Value().Count(); ++at) {
                auto element = list.Value().Element(at);
                auto name = element ? element.Value().ReadText("inputName")
                                    : Result<StringView>::Failure(element.Error());
                auto kind = element
                                ? element.Value().ReadInteger(
                                      "inputType", static_cast<int64>(FunctionInputType::Vector3))
                                : Result<int64>::Failure(element.Error());
                if (!name || !kind || at >= pins.View().Size()) {
                    read = Result<void>::Failure(!name   ? name.Error()
                                                 : !kind ? kind.Error()
                                                         : ErrorCode::InvalidArgument);
                    break;
                }

                read = layerInputs.TryPushBack(
                    LayerStackInputEntry{name.Value(), static_cast<FunctionInputType>(kind.Value()),
                                         pins.View()[at].key});
            }
        }

        if (!read) {
            if (read.Error() == ErrorCode::OutOfMemory) {
                return read;
            }

            const DiagnosticTextPart parts[] = {DiagnosticWord("Node properties cannot be read")};
            if (auto added = diagnose(parts, false); !added) {
                return added;
            }

            return finish();
        }

        auto inputPin = input_.names.TryFind(LayerNodeInputPin);
        for (const nodegraph::GraphEdge& edge : expanded_.edges) {
            if (!stack && inputPin && edge.toNode == owner && edge.toPin == inputPin.Value()) {
                inputEdge.Emplace(edge);
            }
        }
    }

    // 함수 호출 사본 안의 레이어 노드는 쓸 수 없다(MaterialSample 사본은 머티리얼 문맥이다).
    // 레이어 합성 호출도 넘어 끝까지 올라간다(레이어 함수 안의 레이어 노드도 함수 안이다).
    bool insideFunction = false;
    bool parentSampled = false;
    Guid at = owner;
    const ExpandedOrigin* origin = nullptr;
    for (usize step = 0;
         step <= origins_.Count() + syntheticOwners_.Count() && ClimbOrigin(at, origin); ++step) {
        if (origin == nullptr) {
            continue;
        }

        const CallInfo* call = calls_.Find(at);
        insideFunction =
            insideFunction || call == nullptr || !call->sample || syntheticOwners_.Contains(at);
        parentSampled = parentSampled || origin->sampled;
    }

    if (entries.Size() > MaxMaterialLayers) {
        const DiagnosticTextPart parts[] = {
            DiagnosticWord("Material layer count exceeds the limit")};
        if (auto added = diagnose(parts, true); !added) {
            return added;
        }

        return finish();
    }

    // 같은 layerId는 합성 호출 id가 겹친다(Rocks 검사 — UE는 index로 찾는다).
    for (usize at = 0; at < entries.Size(); ++at) {
        for (usize other = 0; other < at; ++other) {
            if (entries[other].layerId == entries[at].layerId) {
                const DiagnosticTextPart parts[] = {
                    DiagnosticWord("Layer "), DiagnosticNumber(at),
                    DiagnosticWord(" has the same layer id as layer "), DiagnosticNumber(other),
                    DiagnosticWord(".")};
                if (auto added = diagnose(parts, true); !added) {
                    return added;
                }
            }
        }
    }

    // snapshot 표시 이름과 시그니처다(없으면 이름 빈 문자열, 시그니처 없음).
    const auto inspect = [&](Guid asset, StringView& name, MaterialFunctionUsage& usage,
                             FunctionSignature& signature, bool& found) noexcept -> Result<void> {
        name = StringView();
        found = false;
        const nodegraph::Graph* graph = nullptr;
        uint32 snapshotIndex = 0;
        auto snapshot = LoadSnapshot(asset, false, graph, snapshotIndex);
        if (!snapshot) {
            return Result<void>::Failure(snapshot.Error());
        }

        if (snapshot.Value() == nullptr) {
            return Result<void>::Success();
        }

        found = true;
        name = snapshot.Value()->name;
        usage = snapshot.Value()->usage;
        return ReadSignature(snapshot.Value()->document, *graph, signature, 0, true);
    };

    usize activeLayers = 0;
    usize activeBlends = 0;
    FunctionSignature inspected(scratch);
    if (stack) {
        // 쓸 수 있는 함수 목록 검사다(UE LayerStack::ValidateLayerConfiguration — 하나라도 실패하면
        // 끝).
        bool availableValid = true;
        for (usize which = 0; which < LayerFunctionRoleCount; ++which) {
            const bool blends = which == 1;
            for (Guid asset : blends ? availableBlends : availableLayers) {
                StringView name;
                MaterialFunctionUsage usage = MaterialFunctionUsage::General;
                bool found = false;
                if (auto read = inspect(asset, name, usage, inspected, found); !read) {
                    return read;
                }

                if (!found) {
                    continue;
                }

                const bool usageValid = blends ? usage == MaterialFunctionUsage::LayerBlend
                                               : usage == MaterialFunctionUsage::General ||
                                                     usage == MaterialFunctionUsage::Layer;
                if (!usageValid) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Function "), DiagnosticWord(name),
                        DiagnosticWord(blends ? ": function usage is not set for use as a blend."
                                              : ": function usage is not set for use as a layer.")};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }

                    availableValid = false;
                    continue;
                }

                if (inspected.hasStatic) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Function "), DiagnosticWord(name),
                        DiagnosticWord(
                            ": cannot contain any static nodes as these are incompatible "
                            "with the layering.")};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }

                    availableValid = false;
                }

                if (inspected.AttributeOutputs() != AcceptableLayerBlendAttributeOutputs) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Function "), DiagnosticWord(name),
                        DiagnosticWord(
                            ", must have exactly 1 Material Attributes type output node. It "
                            "has "),
                        DiagnosticNumber(inspected.AttributeOutputs())};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }

                    availableValid = false;
                }

                const usize inputs = inspected.AttributeInputs();
                if (!blends && inputs > AcceptableLayerAttributeInputs) {
                    // UE 문구 그대로다("output node" — MaterialExpressionLayerStack.cpp:358).
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Layer "), DiagnosticWord(name),
                        DiagnosticWord(
                            ": can only have up to 1 Material Attributes type output node. "
                            "It has "),
                        DiagnosticNumber(inputs)};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }

                    availableValid = false;
                }

                if (blends && inputs != AcceptableBlendAttributeInputs) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Blend "), DiagnosticWord(name),
                        DiagnosticWord(
                            ": must have exactly 2 Material Attributes type input node. It "
                            "has "),
                        DiagnosticNumber(inputs)};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }

                    availableValid = false;
                }
            }
        }

        if (!availableValid) {
            const DiagnosticTextPart parts[] = {
                DiagnosticWord("LayerStack Available Layers/Blends contain invalid functions.")};
            if (auto added = diagnose(parts, true); !added) {
                return added;
            }

            return finish();
        }

        bool inputsConnected = true;
        for (const LayerStackInputEntry& item : layerInputs) {
            bool connected = false;
            for (const nodegraph::GraphEdge& edge : expanded_.edges) {
                connected = connected || (edge.toNode == owner && edge.toPin == item.key);
            }

            if (!connected) {
                const DiagnosticTextPart parts[] = {
                    DiagnosticWord("LayerStack \""), DiagnosticWord(item.name),
                    DiagnosticWord("\" input pin must be connected.")};
                if (auto added = diagnose(parts, true); !added) {
                    return added;
                }

                inputsConnected = false;
            }
        }

        if (!inputsConnected) {
            return finish();
        }

        const auto allowed = [](const Array<Guid>& list, Guid asset) noexcept {
            for (Guid item : list) {
                if (item == asset) {
                    return true;
                }
            }

            return false;
        };

        bool listed = true;
        for (usize at = 0; at < entries.Size(); ++at) {
            const LayerEntry& entry = entries[at];
            for (usize which = 0; which < LayerFunctionRoleCount; ++which) {
                const bool blends = which == 1;
                const Guid asset = blends ? entry.blend : entry.layer;
                if (asset == Guid{} || (blends && at == 0)) {
                    continue;
                }

                if (allowed(blends ? availableBlends : availableLayers, asset)) {
                    const bool layerActive = !(entry.layer == Guid{}) && entry.visible;
                    activeLayers += !blends && layerActive ? 1 : 0;
                    activeBlends += blends && layerActive ? 1 : 0;
                    continue;
                }

                StringView name;
                MaterialFunctionUsage usage = MaterialFunctionUsage::General;
                bool found = false;
                if (auto read = inspect(asset, name, usage, inspected, found); !read) {
                    return read;
                }

                const DiagnosticTextPart parts[] = {
                    DiagnosticWord(blends ? "Blend " : "Layer "),
                    DiagnosticNumber(blends ? at - 1 : at), DiagnosticWord(", "),
                    DiagnosticWord(name),
                    DiagnosticWord(blends
                                       ? ", is not set as an allowed blend in the base layer stack "
                                         "node."
                                       : ", is not set as an allowed layer in the base layer stack "
                                         "node.")};
                if (auto added = diagnose(parts, true); !added) {
                    return added;
                }

                listed = false;
            }
        }

        if (!listed) {
            return finish();
        }

        if (!(activeBlends == 0 || (activeLayers >= 2 && activeBlends == activeLayers - 1))) {
            const DiagnosticTextPart parts[] = {
                DiagnosticWord("Invalid number of layers ("), DiagnosticNumber(activeLayers),
                DiagnosticWord(") or blends ("), DiagnosticNumber(activeBlends),
                DiagnosticWord(
                    ") assigned. Number of blends must be equal to 0, or 1 less than the "
                    "number of active layers.")};
            if (auto added = diagnose(parts, true); !added) {
                return added;
            }
        }
    } else {
        // AttributeLayers 검사다(UE ValidateLayerConfiguration —
        // MaterialExpressions.cpp:7365–7470).
        for (usize at = 0; at < entries.Size(); ++at) {
            const LayerEntry& entry = entries[at];
            if (!(entry.layer == Guid{})) {
                StringView name;
                MaterialFunctionUsage usage = MaterialFunctionUsage::General;
                bool found = false;
                if (auto read = inspect(entry.layer, name, usage, inspected, found); !read) {
                    return read;
                }

                if (found && usage != MaterialFunctionUsage::Layer) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Layer "), DiagnosticNumber(at), DiagnosticWord(", "),
                        DiagnosticWord(name), DiagnosticWord(", not set for layer usage.")};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }
                } else if (found && inspected.inputs.Size() > AcceptableLayerAttributeInputs) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Layer "), DiagnosticNumber(at), DiagnosticWord(", "),
                        DiagnosticWord(name),
                        DiagnosticWord(", must have one MaterialAttributes input only.")};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }
                }

                activeLayers += entry.visible ? 1 : 0;
            }
        }

        for (usize at = 1; at < entries.Size(); ++at) {
            const LayerEntry& entry = entries[at];
            if (!(entry.blend == Guid{})) {
                StringView name;
                MaterialFunctionUsage usage = MaterialFunctionUsage::General;
                bool found = false;
                if (auto read = inspect(entry.blend, name, usage, inspected, found); !read) {
                    return read;
                }

                if (found && usage != MaterialFunctionUsage::LayerBlend) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Blend "), DiagnosticNumber(at - 1), DiagnosticWord(", "),
                        DiagnosticWord(name), DiagnosticWord(", not set for layer blend usage.")};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }
                } else if (found && inspected.inputs.Size() != AcceptableBlendAttributeInputs) {
                    const DiagnosticTextPart parts[] = {
                        DiagnosticWord("Blend "), DiagnosticNumber(at - 1), DiagnosticWord(", "),
                        DiagnosticWord(name),
                        DiagnosticWord(", must have two MaterialAttributes inputs.")};
                    if (auto added = diagnose(parts, true); !added) {
                        return added;
                    }
                }
            }

            // 함수 없는 혼합 자리도 센다(불투명 레이어 — UE와 같다).
            activeBlends += !(entry.layer == Guid{}) && entry.visible ? 1 : 0;
        }

        const bool layout = (activeLayers == 0 && activeBlends == 0) ||
                            (activeLayers == 1 && activeBlends == 0) ||
                            (activeLayers >= 2 && activeBlends == activeLayers - 1);
        if (!layout) {
            const DiagnosticTextPart parts[] = {
                DiagnosticWord("Invalid number of layers ("), DiagnosticNumber(activeLayers),
                DiagnosticWord(") or blends ("), DiagnosticNumber(activeBlends),
                DiagnosticWord(") assigned.")};
            if (auto added = diagnose(parts, true); !added) {
                return added;
            }
        }
    }

    if (insideFunction) {
        const DiagnosticTextPart parts[] = {
            DiagnosticWord("Layer expressions cannot be used within a material function.")};
        if (auto added = diagnose(parts, true); !added) {
            return added;
        }
    }

    if (layerDiagnostics_.Size() > result.firstDiagnostic) {
        return finish();
    }

    // LayerStack layerInputs 배선이다: 호출 입력 중 이름(대소문자 무시 — UE FName)·종류가 같은 것 ←
    // 그 동적 입력의 연결(UE ProcessFunctionCallers — 레이어·혼합 둘 다).
    const auto wireLayerInputs = [&](Guid call,
                                     const FunctionSignature& signature) noexcept -> Result<void> {
        for (const FunctionSignaturePin& pin : signature.inputs) {
            for (const LayerStackInputEntry& item : layerInputs) {
                if (!NameTextEquals(item.name, pin.name) || item.type != pin.type) {
                    continue;
                }

                for (usize edge = 0; edge < expanded_.edges.Size(); ++edge) {
                    const nodegraph::GraphEdge source = expanded_.edges[edge];
                    if (source.toNode == owner && source.toPin == item.key) {
                        if (auto added =
                                AddExpandedEdge(source.fromNode, source.fromPin, call, pin.key);
                            !added) {
                            return added;
                        }

                        break;
                    }
                }
            }
        }

        return Result<void>::Success();
    };

    // 활성 레이어·혼합 합성 호출이다(활성 순 — UE caller 색인).
    Array<Guid> layerCalls(scratch);
    Array<FunctionSignature> layerSignatures(scratch);
    Array<Guid> blendCalls(scratch);
    Array<FunctionSignature> blendSignatures(scratch);
    for (usize at = 0; at < entries.Size(); ++at) {
        const LayerEntry& entry = entries[at];
        if (entry.layer == Guid{} || !entry.visible) {
            continue;
        }

        FunctionSignature signature(scratch);
        auto call = AddLayerCall(owner, entry.layerId, LayerCallRole, entry.layer, parentSampled,
                                 signature);
        if (!call) {
            return Result<void>::Failure(call.Error());
        }

        // 레이어 입력 연결이다: AttributeLayers는 첫 입력 ← Input, LayerStack은 이름·종류가 같은
        // layerInputs ← 그 동적 입력의 연결.
        if (!stack && inputEdge.HasValue() && !signature.inputs.IsEmpty()) {
            if (auto added = AddExpandedEdge(inputEdge->fromNode, inputEdge->fromPin, call.Value(),
                                             signature.inputs[0].key);
                !added) {
                return added;
            }
        }

        if (stack) {
            if (auto wired = wireLayerInputs(call.Value(), signature); !wired) {
                return wired;
            }
        }

        if (auto added = layerCalls.TryPushBack(call.Value()); !added) {
            return added;
        }

        if (auto added = layerSignatures.TryPushBack(Move(signature)); !added) {
            return added;
        }

        // 혼합 자리다(레이어 at ≥ 1이 활성이면 — 함수가 없으면 불투명 레이어).
        if (at == 0 || (stack && entry.blend == Guid{})) {
            continue;
        }

        FunctionSignature blendSignature(scratch);
        Guid blendCall;
        if (!(entry.blend == Guid{})) {
            auto added = AddLayerCall(owner, entry.layerId, BlendCallRole, entry.blend,
                                      parentSampled, blendSignature);
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            blendCall = added.Value();
            if (stack) {
                if (auto wired = wireLayerInputs(blendCall, blendSignature); !wired) {
                    return wired;
                }
            }
        }

        if (auto added = blendCalls.TryPushBack(blendCall); !added) {
            return added;
        }

        if (auto added = blendSignatures.TryPushBack(Move(blendSignature)); !added) {
            return added;
        }
    }

    // from 호출의 첫 출력 → to 호출의 입력 pin이다.
    const auto connect = [&](Guid from, const FunctionSignature& fromSignature,
                             const FunctionSignaturePin* to, Guid toCall) noexcept -> Result<void> {
        if (to == nullptr || fromSignature.outputs.IsEmpty()) {
            return Result<void>::Success();
        }

        return AddExpandedEdge(from, fromSignature.outputs[0].key, toCall, to->key);
    };

    const usize layerCount = layerCalls.Size();
    const usize blendCount = blendCalls.Size();
    Guid finalCall;
    const FunctionSignature* finalSignature = nullptr;
    if (!stack) {
        // UE RebuildLayerGraph 사슬이다(MaterialExpressions.cpp:7310–7330).
        if (layerCount >= 2 && blendCount >= 1) {
            for (usize blend = 0; blend + 1 < layerCount && blend < blendCount; ++blend) {
                if (blendCalls[blend] == Guid{}) {
                    continue;
                }

                const bool previous = blend > 0 && !(blendCalls[blend - 1] == Guid{});
                const Guid bottomCall = blend == 0 ? layerCalls[0]
                                        : previous ? blendCalls[blend - 1]
                                                   : layerCalls[blend];
                const FunctionSignature& bottomSignature = blend == 0 ? layerSignatures[0]
                                                           : previous ? blendSignatures[blend - 1]
                                                                      : layerSignatures[blend];
                const FunctionSignaturePin* bottom = nullptr;
                const FunctionSignaturePin* top = nullptr;
                BlendInputs(blendSignatures[blend], false, bottom, top);
                if (auto added = connect(bottomCall, bottomSignature, bottom, blendCalls[blend]);
                    !added) {
                    return added;
                }

                if (auto added = connect(layerCalls[blend + 1], layerSignatures[blend + 1], top,
                                         blendCalls[blend]);
                    !added) {
                    return added;
                }
            }
        }

        if (blendCount > 0 && !(blendCalls[blendCount - 1] == Guid{})) {
            finalCall = blendCalls[blendCount - 1];
            finalSignature = &blendSignatures[blendCount - 1];
        } else if (layerCount > 0) {
            finalCall = layerCalls[layerCount - 1];
            finalSignature = &layerSignatures[layerCount - 1];
        }
    } else if (blendCount > 0) {
        // LayerStack 혼합 사슬이다(UE MaterialExpressionLayerStack.cpp:617–660 — MaterialAttributes
        // 입력의 relevance만 본다).
        Guid bottomCall = layerCalls[0];
        const FunctionSignature* bottomSignature = &layerSignatures[0];
        for (usize blend = 0; blend < blendCount && blend + 1 < layerCount; ++blend) {
            const FunctionSignaturePin* bottom = nullptr;
            const FunctionSignaturePin* top = nullptr;
            BlendInputs(blendSignatures[blend], true, bottom, top);
            if (auto added = connect(bottomCall, *bottomSignature, bottom, blendCalls[blend]);
                !added) {
                return added;
            }

            if (auto added = connect(layerCalls[blend + 1], layerSignatures[blend + 1], top,
                                     blendCalls[blend]);
                !added) {
                return added;
            }

            bottomCall = blendCalls[blend];
            bottomSignature = &blendSignatures[blend];
        }

        finalCall = blendCalls[blendCount - 1];
        finalSignature = &blendSignatures[blendCount - 1];
    } else if (layerCount > 0) {
        // 혼합이 없으면 위 레이어의 첫 MaterialAttributes 입력 ← 아래 레이어 결과다(6.12절 MS2e3
        // 편차 — UE는 이름이 같은 출력에 잇는다).
        for (usize layer = 1; layer < layerCount; ++layer) {
            const FunctionSignaturePin* target = nullptr;
            for (const FunctionSignaturePin& pin : layerSignatures[layer].inputs) {
                if (target == nullptr && pin.attributes) {
                    target = &pin;
                }
            }

            if (auto added = connect(layerCalls[layer - 1], layerSignatures[layer - 1], target,
                                     layerCalls[layer]);
                !added) {
                return added;
            }
        }

        finalCall = layerCalls[layerCount - 1];
        finalSignature = &layerSignatures[layerCount - 1];
    }

    if (finalSignature == nullptr) {
        result.passThrough = true;
    } else {
        result.output.node = finalCall;
        result.output.pin =
            finalSignature->outputs.IsEmpty() ? Name() : finalSignature->outputs[0].key;
    }

    return finish();
}

Result<IrValueId> Translator::LowerLayers(const nodegraph::GraphNode& node,
                                          bool& passThrough) noexcept {
    passThrough = false;
    const LayerResult* layer = layers_.Find(node.id);
    if (layer == nullptr) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (!layer->valid) {
        for (uint32 at = 0; at < layer->diagnosticCount; ++at) {
            if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                       MaterialDiagnosticCode::LayerStackRule, node.id, Name(),
                                       layerDiagnostics_[layer->firstDiagnostic + at].View());
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    if (layer->passThrough) {
        passThrough = true;
        return Result<IrValueId>::Success(NoIrValue);
    }

    const nodegraph::GraphNode* call = FindNode(layer->output.node);
    if (call == nullptr) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    bool connected = false;
    return LowerCallOutput(*call, layer->output.pin, connected);
}

Result<void> Translator::ReportUsageError(const nodegraph::GraphNode& node) noexcept {
    const CallInfo* call = calls_.Find(node.id);
    if (call == nullptr || call->snapshot >= snapshots_.Size()) {
        return Result<void>::Success();
    }

    const MaterialGraphSnapshot& snapshot = *snapshots_[call->snapshot];
    const bool blend = snapshot.usage == MaterialFunctionUsage::LayerBlend;
    SystemAllocator scratch;
    const auto report = [&](Span<const DiagnosticTextPart> parts) noexcept -> Result<void> {
        String text(scratch);
        Result<void> built = text.TryAssign(LayerValidationPrefix);
        if (built) {
            built = AppendDiagnosticText(text, parts);
        }

        if (!built) {
            return built;
        }

        // 함수 용도 검사는 UE가 PushFunction 뒤에 낸다(ME:14258 — 경로에 그 함수 이름까지).
        return ReportInFunction(nodegraph::GraphDiagnosticSeverity::Error,
                                MaterialDiagnosticCode::LayerStackRule, node.id, Name(),
                                text.View(), call->snapshot);
    };

    if (call->attributeOutputs != AcceptableLayerBlendAttributeOutputs) {
        const DiagnosticTextPart parts[] = {
            DiagnosticWord("Function "), DiagnosticWord(snapshot.name),
            DiagnosticWord(", must have exactly 1 Material Attributes type output node. It has "),
            DiagnosticNumber(call->attributeOutputs)};
        if (auto reported = report(parts); !reported) {
            return reported;
        }
    }

    if (!blend && call->attributeInputs > AcceptableLayerAttributeInputs) {
        const DiagnosticTextPart parts[] = {
            DiagnosticWord("Layer "), DiagnosticWord(snapshot.name),
            DiagnosticWord(": can only have up to 1 Material Attributes type output node. It has "),
            DiagnosticNumber(call->attributeInputs)};
        if (auto reported = report(parts); !reported) {
            return reported;
        }
    }

    if (blend && call->attributeInputs != AcceptableBlendAttributeInputs) {
        const DiagnosticTextPart parts[] = {
            DiagnosticWord("Blend "), DiagnosticWord(snapshot.name),
            DiagnosticWord(": must have exactly 2 Material Attributes type input node. It has "),
            DiagnosticNumber(call->attributeInputs)};
        if (auto reported = report(parts); !reported) {
            return reported;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::material::detail
