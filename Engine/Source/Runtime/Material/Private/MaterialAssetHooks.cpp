#include <Base/Compiler.h>
#include <Core/Move.h>
#include <AssetRuntime/Package.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <Material/MaterialAssetHooks.h>
#include <Material/MaterialFunction.generated.h>
#include <Material/MaterialFunction.h>
#include <Material/MaterialFunctionInstance.h>
#include <Material/MaterialFunctionInstance.generated.h>
#include <Material/MaterialInstance.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackDefaults.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.generated.h>
#include <Material/MaterialInstance.generated.h>

// 머티리얼 에셋 타입 hook이다(Docs/Design/MaterialSystem 4.4·4.6.1절).
namespace rocks {

namespace {

// target이 속한 패키지의 id를 Build 의존으로 더한다(null이면 건너뛴다). 대표 에셋의 Asset::Id는
// 로드 때만 채워지므로(Package.h) 메모리에서 만든 에셋도 맞게 내려고 target의 outermost Package를
// 본다. 대상이 이 에셋과 같은 패키지(context.object의 outermost)면 건너뛴다 — 복제에서도 원본
// 패키지 안 객체를 자기로 본다. 오류: outermost가 Package가 아니거나(패키지 없는 객체 —
// PackageWriter도 저장 실패로 다룬다) 패키지 id가 null이면 InvalidArgument, Registry 조회 오류.
Result<void> AddAssetOf(const AssetTypeHookContext& context, ObjectHandle target,
                        AssetDependencySink& sink) noexcept {
    if (target.IsNull()) {
        return Result<void>::Success();
    }

    auto outermost = context.registry.GetOutermost(context.access, target);
    if (!outermost) {
        return Result<void>::Failure(outermost.Error());
    }

    auto self = context.registry.GetOutermost(context.access, context.object);
    if (!self) {
        return Result<void>::Failure(self.Error());
    }

    if (outermost.Value() == self.Value()) {
        return Result<void>::Success();
    }

    auto type = context.registry.AcquireType(context.access, outermost.Value());
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    if (!type.Value().IsNativeA(NativeTypeKey<Package>())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto package = context.registry.Resolve<Package>(context.access, outermost.Value());
    if (!package) {
        return Result<void>::Failure(package.Error());
    }

    auto pointer = package.Value().Get();
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    const AssetId id = pointer.Value()->Id();
    if (id.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return sink.Add(id);
}

#if ROCKS_WITH_EDITOR_DATA
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
                             Array<AssetId>& out) noexcept {
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

        return id.Value() == Guid{} ? Result<void>::Success()
                                    : out.TryPushBack(AssetId{id.Value()});
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
            if (id && !(id.Value() == Guid{})) {
                if (auto added = out.TryPushBack(AssetId{id.Value()}); !added) {
                    return added;
                }
            }
        }
    }

    return Result<void>::Success();
}

// 그래프 안 함수 호출·MaterialSample·레이어 노드 참조와 ColorRamp 색 커브·CurveAtlasRowParameter
// atlas를 Build 의존으로 더한다(Docs/Design/MaterialSystem 6.12절 MS2e2·MS2e3 — 번역이 펼치는
// 그래프다, 6.13절 MS2f4 — 번역이 읽는 커브다, 6.10절 MS2c5 — 번역이 읽는 행 목록이다, 4.6.1절).
// 자기 참조는 건너뛴다(번역이 그래프면 V3, 커브면 커브 없음으로 진단한다).
Result<void> AddGraphReferences(const AssetTypeHookContext& context,
                                const nodegraph::GraphDocument& document,
                                AssetDependencySink& sink) noexcept {
    NamePool* names = context.registry.Names();
    if (names == nullptr) {
        return Result<void>::Success();
    }

    auto call = names->TryFind("Material.FunctionCall");
    auto sample = names->TryFind("Material.MaterialSample");
    auto layers = names->TryFind("Material.AttributeLayers");
    auto stack = names->TryFind("Material.LayerStack");
    auto ramp = names->TryFind("Material.ColorRamp");
    auto atlasRow = names->TryFind("Material.CurveAtlasRowParameter");
    SystemAllocator scratch;
    Array<AssetId> referenced(scratch);
    for (const nodegraph::Graph& graph : document.graphs) {
        for (const nodegraph::GraphNode& node : graph.nodes) {
            const bool stacked = stack && node.type == stack.Value();
            if (stacked || (layers && node.type == layers.Value())) {
                referenced.Clear();
                if (auto read = LayerReferences(
                        node,
                        stacked ? GeneratedType<material::LayerStackProperties>::Schema()
                                : GeneratedType<material::AttributeLayersProperties>::Schema(),
                        stacked, referenced);
                    !read) {
                    return read;
                }

                for (const AssetId& id : referenced) {
                    if (id == context.asset) {
                        continue;
                    }

                    if (auto added = sink.Add(id); !added) {
                        return added;
                    }
                }

                continue;
            }

            Result<AssetId> asset = Result<AssetId>::Success(AssetId{});
            if (call && node.type == call.Value()) {
                asset = NodeAssetReference(
                    node, GeneratedType<material::FunctionCallProperties>::Schema(),
                    "materialFunction");
            } else if (sample && node.type == sample.Value()) {
                asset = NodeAssetReference(
                    node, GeneratedType<material::MaterialSampleProperties>::Schema(),
                    "materialReference");
            } else if (ramp && node.type == ramp.Value()) {
                asset = NodeAssetReference(
                    node, GeneratedType<material::ColorRampProperties>::Schema(), "colorCurve");
            } else if (atlasRow && node.type == atlasRow.Value()) {
                asset = NodeAssetReference(
                    node, GeneratedType<material::CurveAtlasRowParameterProperties>::Schema(),
                    "atlas");
            } else {
                continue;
            }

            if (!asset) {
                return Result<void>::Failure(asset.Error());
            }

            if (asset.Value().IsNull() || asset.Value() == context.asset) {
                continue;
            }

            if (auto added = sink.Add(asset.Value()); !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

template <typename Asset>
Result<void> GraphBuildDependencies(const AssetTypeHookContext& context,
                                    AssetDependencySink& sink) noexcept {
    if (auto base = context.CallBaseBuildDependencies(sink); !base) {
        return base;
    }

    auto borrow = context.registry.Resolve<Asset>(context.access, context.object);
    if (!borrow) {
        return Result<void>::Failure(borrow.Error());
    }

    auto pointer = borrow.Value().Get();
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    return AddGraphReferences(context, pointer.Value()->graph, sink);
}
#endif

Result<void> InstanceBuildDependencies(const AssetTypeHookContext& context,
                                       AssetDependencySink& sink) noexcept {
    if (auto base = context.CallBaseBuildDependencies(sink); !base) {
        return base;
    }

    auto borrow = context.registry.Resolve<MaterialInstance>(context.access, context.object);
    if (!borrow) {
        return Result<void>::Failure(borrow.Error());
    }

    auto pointer = borrow.Value().Get();
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    const MaterialInstance& instance = *pointer.Value();
    if (auto parent = AddAssetOf(context, instance.parent.Handle(), sink); !parent) {
        return parent;
    }

#if ROCKS_WITH_EDITOR_DATA
    if (instance.overridesLayerStack) {
        for (usize index = 0; index < instance.layerStack.layers.Size(); ++index) {
            const MaterialLayerEntry& entry = instance.layerStack.layers[index];
            if (auto layer = AddAssetOf(context, entry.layer.Handle(), sink); !layer) {
                return layer;
            }

            if (auto blend = AddAssetOf(context, entry.blend.Handle(), sink); !blend) {
                return blend;
            }
        }
    }
#endif

    return Result<void>::Success();
}

Result<void> FunctionInstanceBuildDependencies(const AssetTypeHookContext& context,
                                               AssetDependencySink& sink) noexcept {
    if (auto base = context.CallBaseBuildDependencies(sink); !base) {
        return base;
    }

#if ROCKS_WITH_EDITOR_DATA
    auto borrow =
        context.registry.Resolve<MaterialFunctionInstance>(context.access, context.object);
    if (!borrow) {
        return Result<void>::Failure(borrow.Error());
    }

    auto pointer = borrow.Value().Get();
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    return AddAssetOf(context, pointer.Value()->parent.Handle(), sink);
#else
    return Result<void>::Success();
#endif
}

} // namespace

Result<void> RegisterMaterialAssetHooks(AssetTypeHookTable& table,
                                        Array<AssetTypeHookRegistration>& out) noexcept {
    struct Entry {
        TypeId type;
        AssetBuildDependencyHook hook;
    };

    const Entry entries[] = {
        {GeneratedType<MaterialInstance>::Id(), &InstanceBuildDependencies},
        {GeneratedType<MaterialFunctionInstance>::Id(), &FunctionInstanceBuildDependencies},
#if ROCKS_WITH_EDITOR_DATA
        {GeneratedType<Material>::Id(), &GraphBuildDependencies<Material>},
        {GeneratedType<MaterialFunction>::Id(), &GraphBuildDependencies<MaterialFunction>},
#endif
    };

    const usize first = out.Size();
    for (const Entry& entry : entries) {
        AssetTypeHooks hooks;
        hooks.buildDependencies = entry.hook;
        auto registered = table.Register(entry.type, hooks);
        if (!registered) {
            while (out.Size() > first) {
                (void)out.TryPopBack();
            }

            return Result<void>::Failure(registered.Error());
        }

        if (auto pushed = out.TryPushBack(Move(registered.Value())); !pushed) {
            while (out.Size() > first) {
                (void)out.TryPopBack();
            }

            return pushed;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
