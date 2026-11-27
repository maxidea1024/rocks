#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 선택이 바뀐 까닭이다(UE ESelectInfo — OnKeyPress·OnNavigation·OnMouseClick·Direct). 콤보 상자의
// 선택 사건이 함께 넘긴다: Key는 닫힌 콤보의 위·아래 키(Rocks 확장), Navigation은 탐색(UE처럼
// 사건을 보내지 않는 강조 이동에만 쓴다), Pointer는 목록 행 클릭, Direct는 프로그램 선택·확인 키
// 확정·선택 항목 삭제로 인한 해제다(UE SComboBox — Enter 확정은 Direct, SComboBox.h 487행).
enum class SelectionCause : uint8 {
    Key,
    Navigation,
    Pointer,
    Direct,
    // 끝 표지다(값 개수 — Widget 층 미러가 개수를 대조한다).
    Count
};

} // namespace rocks::ui
