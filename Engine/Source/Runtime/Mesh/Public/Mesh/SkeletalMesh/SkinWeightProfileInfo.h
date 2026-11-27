#pragma once

#include <AssetRuntime/PerPlatformBool.h>
#include <AssetRuntime/PerPlatformInt32.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 스킨 가중치 프로필 정보다(Docs/Design/MeshAnimationAssets 6.3절·6.7.1절 5, UE
// FSkinWeightProfileInfo — E/Public/Rendering/SkinWeightProfile.h:41). 이름은 원본
// SkinWeights-<profile> 속성의 profile이다.
namespace rocks {

// 이름·기본 프로필 여부(플랫폼별)·그때 쓰기 시작할 LOD(플랫폼별, 0 이상)다. UE PerLODSourceFiles는
// 편집기 원본 경로라 넣지 않는다.
//
// 사용 예:
//   SkinWeightProfileInfo info(allocator);
//   info.name = names.TryIntern("Bulky").Value();
//   info.defaultProfile.defaultValue = true;
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkinWeightProfileInfo {
    ROCKS_GENERATED_BODY()

    explicit SkinWeightProfileInfo(Allocator& allocator) noexcept
        : defaultProfile(allocator), defaultProfileFromLod(allocator) {
    }

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() PerPlatformBool defaultProfile;
    ROCKS_PROPERTY() PerPlatformInt32 defaultProfileFromLod;
};

} // namespace rocks
