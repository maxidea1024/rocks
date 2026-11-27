#pragma once

#include <Core/Math/Color.h>

// 공통 기능 탭 속성 바인딩 견본의 모델이다(Docs/Design/UiToolkit 16.9.2절). 슬라이더가 값을 쓰고,
// 상자 색·진행 막대가 Bind(model, &BindingSample::field)로 읽는다. 위젯보다 오래 살도록 탭이
// 소유한다.
namespace rocks::ui::gallery {

struct BindingSample {
    LinearColor tint{1, 1, 1, 1};
    float progress = 0.5f;
};

} // namespace rocks::ui::gallery
