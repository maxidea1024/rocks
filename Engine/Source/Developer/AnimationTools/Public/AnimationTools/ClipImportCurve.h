#pragma once

#include <Core/Text/StringView.h>
#include <Curves/TangentCurve.h>

// 클립 임포트 명세의 커브 하나다(Docs/Design/MeshAnimationAssets 7.6.2절 5 — ClipCurve). curve는
// null이 아니어야 하고 ImportAnimationClip이 끝날 때까지 산다(서비스가 복사한다).
namespace rocks {

struct ClipImportCurve {
    StringView name{};
    bool editable = true;
    bool metadata = false;
    bool disabled = false;
    const TangentCurve* curve = nullptr;
};

} // namespace rocks
