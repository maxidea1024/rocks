#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/StringView.h>

// 클립 임포트 명세의 본 트랙 하나다(Docs/Design/MeshAnimationAssets 7.6.2절 5 — 7.2절 BoneTrack).
// 성분마다 원소 1개(상수) 또는 키 수(frameCount + 1)개다. span은 ImportAnimationClip이 끝날 때까지
// 산다.
namespace rocks {

struct ClipImportTrack {
    StringView bone{};
    Span<const Vec3> translation{};
    Span<const Quat> rotation{};
    Span<const Vec3> scale{};
};

} // namespace rocks
