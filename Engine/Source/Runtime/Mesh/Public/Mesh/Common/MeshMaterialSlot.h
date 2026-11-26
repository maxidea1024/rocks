#pragma once

#include <Base/Compiler.h>
#include <Core/Text/Name.h>
#include <Material/Material.h>
#include <Mesh/Common/MeshUvDensity.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>

// 메시 머티리얼 슬롯이다(Docs/Design/MeshAnimationAssets 4.3절·5.5.1절 4, UE FStaticMaterial·
// FSkeletalMaterial — StaticMesh.h·SkinnedAssetCommon.h). 정적 메시와 스킨 메시가 함께 쓴다.
namespace rocks {

// 머티리얼 슬롯 하나다(참조 struct — 슬롯 배열은 StructRefArray<MeshMaterialSlot>). 다각형 묶음의
// MaterialSlotName이 importedSlotName과 맞는 슬롯을 그 섹션이 쓴다(Name 비교 규칙, 4.3절).
//
// 사용 예:
//   ROCKS_PROPERTY() StructRefArray<MeshMaterialSlot> materialSlots;   // 메시 필드
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다(참조 필드 쓰기는 Registry가 한다).
ROCKS_STRUCT()
struct MeshMaterialSlot {
    ROCKS_GENERATED_BODY()

    // 사용자에게 보이는 이름이다(UE MaterialSlotName — 게임 코드가 슬롯을 이름으로 찾는다).
    ROCKS_PROPERTY() Name slotName;
#if ROCKS_WITH_EDITOR_DATA
    // 임포트한 원본의 슬롯 키다(UE ImportedMaterialSlotName — 재임포트가 슬롯 순서를 맞춘다).
    ROCKS_PROPERTY(EditorOnly) Name importedSlotName;
#endif
    // 머티리얼이다(Hard). 비면 엔진 기본 머티리얼이다.
    ROCKS_PROPERTY() ObjectRef<MaterialBase> material;
    // 겹쳐 그리는 머티리얼이다(Hard, UE OverlayMaterialInterface). 비어도 된다.
    ROCKS_PROPERTY() ObjectRef<MaterialBase> overlayMaterial;
    ROCKS_PROPERTY() MeshUvDensity uvDensity;
};

} // namespace rocks
