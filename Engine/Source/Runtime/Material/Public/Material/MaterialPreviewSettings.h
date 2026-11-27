#pragma once

#include <Base/Types.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <Meta/Annotations.h>

// 머티리얼 에디터 미리보기 설정이다(Docs/Design/MaterialSystem 11.4·19.9절, UE
// EditorPitch·EditorYaw· PreviewMesh). EditorOnly 필드로만 쓴다.
namespace rocks {

ROCKS_STRUCT()
struct MaterialPreviewSettings {
    ROCKS_GENERATED_BODY()

    // 미리보기 메시(비면 엔진 기본 구)다.
    ROCKS_PROPERTY() material::MaterialAssetReference mesh;
    // 미리보기 카메라 회전(도, UE EditorPitch·EditorYaw).
    ROCKS_PROPERTY() int32 cameraPitch = 0;
    ROCKS_PROPERTY() int32 cameraYaw = 0;
};

} // namespace rocks
