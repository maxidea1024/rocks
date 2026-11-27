#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 연결 없는 입력 핀에 사용자가 넣은 값이다(Docs/Design/MaterialSystem 5.3·6.8절). NodeGraph
// PinValueEntry.value Record의 struct다. 성분은 늘 float로 담고(UE 입력 덮기 상수도 float다),
// 번역이 핀 타입으로 바꾼다. componentCount는 1–4이고 타입이 정해진 핀이면 그 폭 이하다. 유한하지
// 않은 성분은 번역의 V11 오류다.
//
// 사용 예(Float3 핀에 회색):
//   MaterialPinValue value;
//   value.x = value.y = value.z = 0.5f;
//   value.componentCount = 3;
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialPinValue {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float x = 0.0f;
    ROCKS_PROPERTY() float y = 0.0f;
    ROCKS_PROPERTY() float z = 0.0f;
    ROCKS_PROPERTY() float w = 0.0f;
    // 쓰는 성분 수(1–4)다. 0이면 값이 없는 것과 같다(번역이 InvalidPinValue로 거부한다).
    ROCKS_PROPERTY() uint8 componentCount = 1;
};

} // namespace rocks::material
