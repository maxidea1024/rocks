#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 편집 글자를 확정한 까닭이다(UE ETextCommit::OnEnter·OnUserMovedFocus — Docs/Design/UiToolkit
// 21.2절).
enum class TextCommitReason : uint8 {
    Enter,
    FocusLost,
    // 끝 표지다(값 개수 — Widget 층 미러가 개수를 대조한다).
    Count
};

} // namespace rocks::ui
