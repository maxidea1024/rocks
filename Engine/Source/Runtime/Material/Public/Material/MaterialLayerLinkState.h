#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 인스턴스 레이어와 부모 레이어의 연결 상태다(Docs/Design/MaterialSystem 4.6.1절, UE
// EMaterialLayerLinkState — RE/Public/Materials/MaterialLayersFunctions.h:21–27). 값은 UE와 같다.
namespace rocks {

ROCKS_ENUM()
enum class MaterialLayerLinkState : uint8 {
    // UE Uninitialized: 옛 엔진 버전으로 저장한 값.
    Uninitialized = 0,
    // 부모 레이어의 변경을 따른다.
    LinkedToParent = 1,
    // 부모에서 왔지만 변경을 따르지 않는다.
    UnlinkedFromParent = 2,
    // 이 인스턴스에서 만든 레이어다.
    NotFromParent = 3
};

} // namespace rocks
