#include <Base/Compiler.h>
#include <Core/Move.h>
#include <AssetRuntime/Package.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <Material/MaterialAssetHooks.h>
#include <Material/MaterialGraphReferences.h>
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
// 그래프 안 Build 의존 참조(함수 호출·MaterialSample·레이어·ColorRamp 색
// 커브·CurveAtlasRowParameter atlas — 노드 정의의 참조 선언, 19.4.3절)를
// 더한다(Docs/Design/MaterialSystem 6.12절 MS2e2·MS2e3 — 번역이 펼치는 그래프다, 6.13절
// MS2f4·6.10절 MS2c5 — 번역이 읽는 커브·행 목록이다, 4.6.1절). 텍스처· 컬렉션은 Hard
// 필드(referencedTextures·parameterCollections)라 여기서 더하지 않는다. 자기 참조는 건너뛴다(번역이
// 그래프면 V3, 커브면 커브 없음으로 진단한다).
Result<void> AddGraphReferences(const AssetTypeHookContext& context,
                                const nodegraph::GraphDocument& document,
                                AssetDependencySink& sink) noexcept {
    NamePool* names = context.registry.Names();
    if (names == nullptr) {
        return Result<void>::Success();
    }

    SystemAllocator scratch;
    Array<MaterialGraphReference> references(scratch);
    if (auto collected = CollectMaterialGraphReferences(*names, document, references); !collected) {
        return collected;
    }

    for (const MaterialGraphReference& reference : references) {
        if (reference.role == material::MaterialAssetReferenceRole::Texture ||
            reference.role == material::MaterialAssetReferenceRole::Collection ||
            reference.asset == context.asset) {
            continue;
        }

        if (auto added = sink.Add(reference.asset); !added) {
            return added;
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
