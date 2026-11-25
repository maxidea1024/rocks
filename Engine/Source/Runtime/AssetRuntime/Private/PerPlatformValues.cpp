#include <AssetRuntime/PerPlatformFloat.h>
#include <AssetRuntime/PerPlatformInt32.h>

// 대상 프로파일별 값이다(Docs/Design/MeshAnimationAssets 5.5.1절 2).
namespace rocks {

float PerPlatformFloat::Resolve(Name profile) const noexcept {
    const float* found = profile.IsNone() ? nullptr : perPlatform.Find(profile);
    return found != nullptr ? *found : defaultValue;
}

int32 PerPlatformInt32::Resolve(Name profile) const noexcept {
    const int32* found = profile.IsNone() ? nullptr : perPlatform.Find(profile);
    return found != nullptr ? *found : defaultValue;
}

} // namespace rocks
