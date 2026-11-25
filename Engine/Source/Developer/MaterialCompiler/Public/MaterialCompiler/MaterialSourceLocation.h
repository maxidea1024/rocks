#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>

// 생성 원문 한 줄의 원인 노드다(Docs/Design/MaterialSystem 7.6·7.7절 원문 위치 지도). 하위 컴파일러
// 오류의 줄 번호를 그래프 노드로 되돌릴 때 쓴다.
namespace rocks::material {

struct MaterialSourceLocation {
    // 생성 파일의 줄 번호(1부터)다.
    uint32 line = 0;
    // 그 줄의 값을 처음 낸 노드다(IR 원인 위치 — 6.8절).
    Guid node;
};

} // namespace rocks::material
