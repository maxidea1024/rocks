#pragma once

#include <Core/Text/StringView.h>

// 스킨 메시 임포트 명세의 대상 프로파일별 bool 값 하나다(Docs/Design/MeshAnimationAssets 6.7.3절
// 5 — PerPlatformBool의 perPlatform 항목, profile은 TargetProfile id).
namespace rocks {

struct SkeletalMeshImportBool {
    StringView profile{};
    bool value = false;
};

} // namespace rocks
