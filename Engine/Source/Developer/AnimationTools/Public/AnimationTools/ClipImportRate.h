#pragma once

#include <Core/Text/StringView.h>
#include <Core/Time/FrameRate.h>

// 클립 임포트 명세의 대상 프로파일별 목표 비율 하나다(Docs/Design/MeshAnimationAssets 7.6.2절 5 —
// PlatformFrameRate::overrides, profile은 TargetProfile id).
namespace rocks {

struct ClipImportRate {
    StringView profile{};
    FrameRate rate = FrameRate::TryCreate(30, 1).Value();
};

} // namespace rocks
