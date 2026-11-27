#pragma once

#include <Base/Guid.h>
#include <Core/Text/StringView.h>

// 머티리얼 IR 값의 원인 위치다(Docs/Design/MaterialSystem 6.8·7.6절). 빌더가 값을 만들 때 붙이고
// 진단·원문 위치 지도가 쓴다.
namespace rocks::material {

// 값을 처음 낸 그래프 위치다(진단·원문 위치 지도 — 7.6절). pin은 출력 핀 키의 정적 원문이다.
// hash-consing 키에 넣지 않는다.
struct IrSite {
    Guid node;
    StringView pin;
};

} // namespace rocks::material
