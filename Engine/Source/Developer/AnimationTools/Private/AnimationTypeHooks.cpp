#include <AnimationTools/AnimationTypeHooks.h>

#include <Animation/AnimationClip.generated.h>
#include <AssetRuntime/Package.generated.h>
#include <Object/ObjectRegistry.h>

// 애니메이션 에셋 타입 hook이다(Docs/Design/MeshAnimationAssets 7.6.1절 2).
namespace rocks {
namespace {

// skeleton이 비어 있지 않으면 그 AssetId를 Build 의존으로 더한다(부모 hook도 부른다).
Result<void> AnimationBuildDependencies(const AssetTypeHookContext& context,
                                        AssetDependencySink& sink) noexcept {
    if (auto base = context.CallBaseBuildDependencies(sink); !base) {
        return base;
    }

    auto borrowed = context.registry.Resolve<AnimationAsset>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    const ObjectHandle skeleton = value.Value()->skeleton.Handle();
    if (skeleton.IsNull()) {
        return Result<void>::Success();
    }

    // 참조 대상의 바깥 끝 패키지 id다(writer의 import 판정과 같다 — 대표 에셋이 아니어도 된다).
    // 패키지 밖 객체(Transient 등)·자기 패키지면 Build 의존이 아니다.
    auto outermost = context.registry.GetOutermost(context.access, skeleton);
    if (!outermost) {
        return Result<void>::Failure(outermost.Error());
    }

    auto type = context.registry.AcquireType(context.access, outermost.Value());
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    if (!type.Value().IsA(TypeIdOf<Package>())) {
        return Result<void>::Success();
    }

    auto package = context.registry.Resolve<Package>(context.access, outermost.Value());
    if (!package) {
        return Result<void>::Failure(package.Error());
    }

    auto packageValue = package.Value().Get();
    if (!packageValue) {
        return Result<void>::Failure(packageValue.Error());
    }

    const AssetId id = packageValue.Value()->Id();
    return id == context.asset || id.IsNull() ? Result<void>::Success() : sink.Add(id);
}

} // namespace

Result<void> RegisterAnimationTypeHooks(void* state, AssetTypeHookTable& table,
                                        Array<AssetTypeHookRegistration>& tokens) noexcept {
    (void)state;
    if (auto reserved = tokens.TryReserve(tokens.Size() + 1); !reserved) {
        return reserved;
    }

    AssetTypeHooks hooks;
    hooks.buildDependencies = &AnimationBuildDependencies;
    auto registered = table.Register(TypeIdOf<AnimationAsset>(), hooks);
    if (!registered) {
        return Result<void>::Failure(registered.Error());
    }

    (void)tokens.TryPushBack(Move(registered.Value()));
    return Result<void>::Success();
}

} // namespace rocks
