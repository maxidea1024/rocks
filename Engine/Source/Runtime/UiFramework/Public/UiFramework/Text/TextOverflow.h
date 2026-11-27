#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 레이블 글자가 받은 영역을 넘칠 때의 처리다(UE ETextOverflowPolicy + Rocks 추가 — Docs/Design/
// UiToolkit 9.1절 UW4o). 말줄임 표지는 호출자가 shaping해 TextLabel::SetEllipsisMarker로
// 준다(표지가 없으면 Clip처럼 동작한다). 자르기는 시각 순서·클러스터 단위다(결합 글자를 나누지
// 않고, 오른쪽에서 왼쪽으로 쓰는 단락은 논리 끝이 시각 왼쪽이다).
enum class TextOverflow : uint8 {
    // 표지 없이 둔다(UE Clip). 경계 밖 글자는 요소의 경계 자르기 설정을 따른다.
    Clip,
    // 폭을 넘는 줄마다 끝을 자르고 표지를 둔다(UE Ellipsis).
    End,
    // End에 더해 높이를 넘으면 마지막으로 온전히 보이는 줄 끝에도 표지를 둔다(UE
    // MultilineEllipsis).
    MultiLineEnd,
    // 폭을 넘는 줄의 가운데를 표지로 바꿔 앞·뒤를 남긴다(UE MiddleEllipsis — 여러 줄·BiDi에도).
    // 높이를 넘으면 마지막 줄 끝에 표지를 둔다.
    Middle,
    // 폭을 넘는 줄의 앞을 표지로 바꿔 끝을 남긴다(경로처럼 끝이 중요한 글자 — UE에 없는 Rocks
    // 추가). 높이를 넘으면 마지막 줄 끝에 표지를 둔다.
    Start
};

} // namespace rocks::ui
