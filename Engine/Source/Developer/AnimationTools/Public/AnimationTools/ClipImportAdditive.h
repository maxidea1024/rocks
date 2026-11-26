#pragma once

#include <Animation/AnimationClip.h>
#include <Object/AssetId.h>

// 클립 임포트 명세의 additive 설정이다(Docs/Design/MeshAnimationAssets 7.6.2절 5 —
// AdditiveSettings, 기준 클립은 AssetId — null이면 비움).
namespace rocks {

struct ClipImportAdditive {
    AdditiveType type = AdditiveType::None;
    AdditiveBasePose basePose = AdditiveBasePose::None;
    AssetId basePoseClip{};
    int32 basePoseFrame = 0;
};

} // namespace rocks
