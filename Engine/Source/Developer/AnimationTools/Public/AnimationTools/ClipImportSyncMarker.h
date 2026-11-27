#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 클립 임포트 명세의 동기 마커 하나다(Docs/Design/MeshAnimationAssets 7.6.2절 5 — ClipSyncMarker).
namespace rocks {

struct ClipImportSyncMarker {
    StringView name{};
    float time = 0.0f;
    int32 track = 0;
};

} // namespace rocks
