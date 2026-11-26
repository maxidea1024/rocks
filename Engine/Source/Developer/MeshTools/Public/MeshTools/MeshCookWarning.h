#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Base/Types.h>

// 메시 쿠킹 경고 하나다(Docs/Design/MeshAnimationAssets 5.5.2절 10·6.7.2절 7 — 정적·스킨 너겟
// 머리가 담는다. 경고는 원본에서 나와 적중이면 다시 계산할 수 없다).
namespace rocks {

struct MeshCookWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    // 쿠킹 LOD 번호다.
    uint32 lod = 0;
};

} // namespace rocks
