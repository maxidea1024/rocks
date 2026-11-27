#include <UnrealExchange/ExchangeSkeletonSettings.h>

#include "ExchangeMeshSettingTables.h"

// 스켈레톤 설정 번역 표다(Docs/Design/UnrealExchange 6.4절 1번).

namespace rocks {

namespace exchange_skeleton_settings {

using namespace exchange_detail;
using Kind = ExchangeSettingKind;

// UE FVirtualBone이다.
constexpr ExchangeSettingRow VirtualBoneMembers[] = {
    Row("SourceBoneName"),
    Row("TargetBoneName"),
    Row("VirtualBoneName"),
};

// 6.4절 1번 표 순서다(행 번호 = 배열 번호 — ExchangeSkeletonSettingRow).
constexpr ExchangeSettingRow SkeletonRows[] = {
    // 0–1: 명세로 옮김, 2: 코드가 본다(커브 메타데이터가 아닌 원소면 경고 — 커브는 data).
    StructRow("VirtualBones", Kind::StructArray, VirtualBoneMembers),
    StructRow("Sockets", Kind::StructArray, BoneSocketMembers),
    Row("AssetUserData", Kind::Ignore),
    // 3–8: 설정이 아님(리타깃 방식은 data, 편집기 미리보기).
    Row("BoneTree", Kind::Ignore),
    Row("PreviewForwardAxis", Kind::Ignore),
    Row("VirtualBoneGuid", Kind::Ignore),
    Row("PreviewSkeletalMesh", Kind::Ignore),
    Row("AdditionalPreviewSkeletalMeshes", Kind::Ignore),
    Row("PreviewAttachedAssetContainer", Kind::Ignore),
    // 9–14: Rocks 필드가 없다(UE 기본값 — USkeleton 생성자).
    UnsupportedRow("CompatibleSkeletons", "[]"),
    UnsupportedRow("bUseRetargetModesFromCompatibleSkeleton", "false"),
    UnsupportedRow("BlendProfiles", "[]"),
    UnsupportedRow("SlotGroups", "[]"),
    UnsupportedRow("AnimationNotifies", "[]"),
    UnsupportedRow("AssetUserDataEditorOnly", "[]"),
};

static_assert(sizeof(SkeletonRows) / sizeof(SkeletonRows[0]) == ExchangeSkeletonSettingRowCount,
              "skeleton setting rows must match ExchangeSkeletonSettingRowCount");

} // namespace exchange_skeleton_settings

Span<const ExchangeSettingRow> ExchangeSkeletonSettingRows() noexcept {
    using namespace exchange_skeleton_settings;
    return Span<const ExchangeSettingRow>(SkeletonRows);
}

} // namespace rocks
