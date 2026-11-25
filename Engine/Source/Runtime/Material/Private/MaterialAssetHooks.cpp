#include <Base/Compiler.h>
#include <Core/Move.h>
#include <AssetRuntime/Package.generated.h>
#include <Material/MaterialAssetHooks.h>
#include <Material/MaterialFunctionInstance.h>
#include <Material/MaterialFunctionInstance.generated.h>
#include <Material/MaterialInstance.h>
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
