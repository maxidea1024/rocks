#include <Core/Memory/SystemAllocator.h>
#include <Material/MaterialGraphReferences.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackDefaults.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.generated.h>

// 그래프 에셋 참조 수집이다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언").
namespace rocks {

namespace {

using material::MaterialAssetReferenceRole;

Result<void> Add(Array<MaterialGraphReference>& out, MaterialAssetReferenceRole role,
                 Guid id) noexcept {
    if (id == Guid{}) {
        return Result<void>::Success();
    }

    return out.TryPushBack(MaterialGraphReference{role, AssetId{id}});
}

// 그래프 노드 하나가 참조하는 에셋 id다(field는 MaterialAssetReference 칸 — 읽지 못하면 null,
// 번역이 진단한다). 오류: 할당 실패.
Result<AssetId> NodeAssetReference(const nodegraph::GraphNode& node, const TypeSchema& schema,
                                   StringView field) noexcept {
    SystemAllocator scratch;
    auto reader = material::NodePropertyReader::Open(scratch, node.properties.bytes.View(), schema);
    auto reference = reader ? reader.Value().OpenStruct(
                                  field, GeneratedType<material::MaterialAssetReference>::Schema())
                            : Result<material::NodePropertyReader>::Failure(reader.Error());
    auto id = reference ? reference.Value().ReadGuid("assetId", Guid{})
                        : Result<Guid>::Failure(reference.Error());
    if (!id) {
        return id.Error() == ErrorCode::OutOfMemory ? Result<AssetId>::Failure(id.Error())
                                                    : Result<AssetId>::Success(AssetId{});
    }

    return Result<AssetId>::Success(AssetId{id.Value()});
}

// 레이어 노드(AttributeLayers·LayerStack)의 레이어·혼합 함수와 LayerStack 쓸 수 있는 목록의 에셋
// id를 out에 더한다(읽지 못하는 칸은 건너뛴다 — 번역이 진단한다). 오류: 할당 실패.
Result<void> LayerReferences(const nodegraph::GraphNode& node, const TypeSchema& schema, bool stack,
                             Array<MaterialGraphReference>& out) noexcept {
    SystemAllocator scratch;
    auto reader = material::NodePropertyReader::Open(scratch, node.properties.bytes.View(), schema);
    if (!reader) {
        return reader.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(reader.Error())
                                                        : Result<void>::Success();
    }

    const auto add = [&](material::NodePropertyReader& element, StringView field) noexcept {
        auto reference =
            element.OpenStruct(field, GeneratedType<material::MaterialAssetReference>::Schema());
        auto id = reference ? reference.Value().ReadGuid("assetId", Guid{})
                            : Result<Guid>::Failure(reference.Error());
        if (!id) {
            return id.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(id.Error())
                                                        : Result<void>::Success();
        }

        return Add(out, MaterialAssetReferenceRole::Function, id.Value());
    };

    auto defaults = reader.Value().OpenStruct(
        "defaultLayers", GeneratedType<material::LayerStackDefaults>::Schema());
    auto layers = defaults ? defaults.Value().OpenStructArray(
                                 "layers", GeneratedType<material::LayerStackLayer>::Schema())
                           : Result<material::NodePropertyList>::Failure(defaults.Error());
    for (usize at = 0; layers && at < layers.Value().Count(); ++at) {
        auto element = layers.Value().Element(at);
        if (!element) {
            break;
        }

        const StringView fields[] = {"layer", "blend"};
        for (StringView field : fields) {
            if (auto added = add(element.Value(), field); !added) {
                return added;
            }
        }
    }

    if (!stack) {
        return Result<void>::Success();
    }

    const StringView lists[] = {"availableLayers", "availableBlends"};
    for (StringView list : lists) {
        auto entries = reader.Value().OpenStructArray(
            list, GeneratedType<material::MaterialAssetReference>::Schema());
        for (usize at = 0; entries && at < entries.Value().Count(); ++at) {
            auto element = entries.Value().Element(at);
            auto id = element ? element.Value().ReadGuid("assetId", Guid{})
                              : Result<Guid>::Failure(element.Error());
            if (id) {
                if (auto added = Add(out, MaterialAssetReferenceRole::Function, id.Value());
                    !added) {
                    return added;
                }
            }
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> CollectMaterialGraphReferences(const NamePool& names,
                                            const nodegraph::GraphDocument& document,
                                            Array<MaterialGraphReference>& out) noexcept {
    auto layers = names.TryFind("Material.AttributeLayers");
    auto stack = names.TryFind("Material.LayerStack");
    for (const nodegraph::Graph& graph : document.graphs) {
        for (const nodegraph::GraphNode& node : graph.nodes) {
            const bool stacked = stack && node.type == stack.Value();
            if (stacked || (layers && node.type == layers.Value())) {
                if (auto read = LayerReferences(
                        node,
                        stacked ? GeneratedType<material::LayerStackProperties>::Schema()
                                : GeneratedType<material::AttributeLayersProperties>::Schema(),
                        stacked, out);
                    !read) {
                    return read;
                }

                continue;
            }

            // 엔진 노드 정의의 참조 선언이다(선언한 정의만 이름을 찾는다).
            for (const Span<const material::MaterialNodeDefinition> group :
                 material::BuiltinMaterialNodeGroups()) {
                for (const material::MaterialNodeDefinition& definition : group) {
                    if (definition.assetReferences.IsEmpty() ||
                        definition.propertiesSchema == nullptr) {
                        continue;
                    }

                    auto type = names.TryFind(definition.type);
                    if (!type || !(node.type == type.Value())) {
                        continue;
                    }

                    for (const material::MaterialAssetReferenceField& field :
                         definition.assetReferences) {
                        auto asset =
                            NodeAssetReference(node, definition.propertiesSchema(), field.field);
                        if (!asset) {
                            return Result<void>::Failure(asset.Error());
                        }

                        if (auto added = Add(out, field.role, asset.Value().value); !added) {
                            return added;
                        }
                    }
                }
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
