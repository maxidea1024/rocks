#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <Curves/TangentCurve.h>

// 클립 임포트 명세의 변환 커브 하나다(Docs/Design/MeshAnimationAssets 7.6.2절 5 —
// ClipTransformCurve). channels는 이동 x·y·z, 회전 x·y·z, 크기 x·y·z 순
// ClipTransformCurveChannels개다.
namespace rocks {

struct ClipImportTransformCurve {
    StringView name{};
    bool editable = true;
    bool metadata = false;
    bool disabled = false;
    Span<const TangentCurve> channels{};
};

} // namespace rocks
