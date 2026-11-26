#include "ProductAssetSetup.h"

#include <AnimationTools/AnimationTypeHooks.h>
#include <TextureTools/TextureImport.h>

// 제품 명령 실행 파일의 에셋 처리기·타입 hook 묶음이다(Docs/Design/MeshAnimationAssets 13.1절 1).
namespace rocks {

Result<void> RegisterProductCookProcessors(void* state, CookProcessorTable& table) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ProductCookProcessors& processors = *static_cast<ProductCookProcessors*>(state);
    if (auto registered = RegisterTextureCookProcessors(&processors.texture, table); !registered) {
        return registered;
    }

    if (auto registered = RegisterStaticMeshCookProcessors(&processors.staticMesh, table);
        !registered) {
        return registered;
    }

    if (auto registered = RegisterSkeletalMeshCookProcessors(&processors.skeletalMesh, table);
        !registered) {
        return registered;
    }

    return RegisterAnimationClipCookProcessors(&processors.animationClip, table);
}

Result<void> RegisterProductTypeHooks(void* state, AssetTypeHookTable& table,
                                      Array<AssetTypeHookRegistration>& tokens) noexcept {
    if (auto registered = RegisterTextureTypeHooks(state, table, tokens); !registered) {
        return registered;
    }

    return RegisterAnimationTypeHooks(state, table, tokens);
}

} // namespace rocks
