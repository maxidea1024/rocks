#pragma once

#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// PostProcess 머티리얼의 사용자 scene texture 이름 바꿈이다(Docs/Design/MaterialSystem 19.11절, UE
// UserSceneTextureOverrides). 효과는 Renderer 후처리 단계가 구현한다.
namespace rocks {

ROCKS_STRUCT()
struct UserSceneTextureOverride {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name key;
    ROCKS_PROPERTY() Name value;
};

} // namespace rocks
