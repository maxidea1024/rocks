#pragma once

#include <Meta/Annotations.h>
#include <UiFramework/Controls/CheckBox.h>
#include <UiFramework/Controls/SpinBox.h>
#include <UiFramework/Controls/TextCommitReason.h>

// UiFramework 값 형식의 리플렉션 미러 선언이다(Docs/Design/UiToolkit 21.5a절 — CoreTypeBindings와
// 같은 Binds 방식, UE NoExportTypes). UiCore·UiFramework는 Object 층에 의존하지 않아 리플렉션이
// 없으므로 Widget 이벤트 인자·속성으로 쓰는 형식을 여기서 싣는다. 미러는 실제 형식과 같은 이름·값·
// 순서의 enumerator를 나열한다(다르면 생성 헤더가 컴파일되지 않는다). 미러 형식(…Binding)을 필드
// 형식으로 직접 쓰지 않는다 — 실제 형식을 쓴다.
//
// 사용 예:
//   ROCKS_PROPERTY(Transient, Params = "state") Event<CheckState> stateChanged_;
namespace rocks::ui_type_bindings {

// 체크 상자 상태다(UE ECheckBoxState).
ROCKS_ENUM(Binds = "rocks::ui::CheckState")
enum class CheckStateBinding : uint8 {
    Unchecked = 0,
    Checked = 1,
    Indeterminate = 2,
};

// 편집 글자를 확정한 까닭이다(UE ETextCommit OnEnter·OnUserMovedFocus).
ROCKS_ENUM(Binds = "rocks::ui::TextCommitReason")
enum class TextCommitReasonBinding : uint8 {
    Enter = 0,
    FocusLost = 1,
};

// 숫자 입력을 확정한 까닭이다.
ROCKS_ENUM(Binds = "rocks::ui::SpinCommitReason")
enum class SpinCommitReasonBinding : uint8 {
    Enter = 0,
    FocusLost = 1,
};

} // namespace rocks::ui_type_bindings
