#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <MeshTools/SkeletalMeshImportBool.h>
#include <MeshTools/StaticMeshImport.h>

// 스킨 메시 임포트 명세의 가중치 프로필 정보 하나다(Docs/Design/MeshAnimationAssets 6.7.3절 5 —
// SkinWeightProfileInfo, UE FSkinWeightProfileInfo). 프로필 가중치 자체는 원본의
// SkinWeights-<name> 속성이다.
namespace rocks {

struct SkeletalMeshImportProfile {
    StringView name{};
    bool defaultProfile = false;
    Span<const SkeletalMeshImportBool> defaultProfilePerPlatform{};
    int32 defaultProfileFromLod = 0;
    Span<const StaticMeshImportInt> defaultProfileFromLodPerPlatform{};
};

} // namespace rocks
