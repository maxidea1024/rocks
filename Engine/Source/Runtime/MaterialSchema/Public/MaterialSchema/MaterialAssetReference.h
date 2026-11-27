#pragma once

#include <Core/Text/Name.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>

// 노드 속성·TODO 필드 안의 에셋 참조다(Docs/Design/MaterialSystem 19.4.3절). 대상 클래스가 아직
// 없어도 같은 형식이라, 클래스가 생겨도 속성 struct migration이 없다. 번역은 snapshot provider로
// 대상을 찾는다.
//
// 사용 예:
//   MaterialAssetReference profile;
//   profile.assetId = id;
//   profile.expectedClass = names.TryIntern("SubsurfaceProfile").Value();
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialAssetReference {
    ROCKS_GENERATED_BODY()

    // null이면 참조 없음이다.
    ROCKS_PROPERTY() AssetId assetId;
    // 기대 클래스의 도구 이름이다(TypeSchema::toolName, 예: "Texture2D").
    ROCKS_PROPERTY() Name expectedClass;

    bool IsNull() const noexcept {
        return assetId.IsNull();
    }
};

} // namespace rocks::material
