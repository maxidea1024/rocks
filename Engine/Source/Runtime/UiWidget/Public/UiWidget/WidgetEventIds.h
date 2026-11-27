#pragma once
#include <Core/Guid.h>

// 공개한 이벤트와 인자 목록의 영속 ID다. 에셋 호환성을 위해 기존 항목의 값을 유지한다.
namespace rocks::ui::widgetEvents {

inline constexpr Guid ButtonClicked{0x25c81f56706c426cULL, 0xa6ea90564a921200ULL};
inline constexpr Guid CheckBoxChanged{0x25c81f56706c426cULL, 0xa6ea90564a921201ULL};
inline constexpr Guid CheckBoxStateChanged{0x25c81f56706c426cULL, 0xa6ea90564a921202ULL};
inline constexpr Guid ComboSelectionChanged{0x25c81f56706c426cULL, 0xa6ea90564a921203ULL};
inline constexpr Guid TextChanged{0x25c81f56706c426cULL, 0xa6ea90564a921204ULL};
inline constexpr Guid TextCommitted{0x25c81f56706c426cULL, 0xa6ea90564a921205ULL};
inline constexpr Guid ExpansionChanged{0x25c81f56706c426cULL, 0xa6ea90564a921206ULL};
inline constexpr Guid InputKeySelected{0x25c81f56706c426cULL, 0xa6ea90564a921207ULL};
inline constexpr Guid RadialSliderChanged{0x25c81f56706c426cULL, 0xa6ea90564a921208ULL};
inline constexpr Guid ScrollBarScrolled{0x25c81f56706c426cULL, 0xa6ea90564a921209ULL};
inline constexpr Guid SliderChanged{0x25c81f56706c426cULL, 0xa6ea90564a92120aULL};
inline constexpr Guid SpinBoxChanged{0x25c81f56706c426cULL, 0xa6ea90564a92120bULL};
inline constexpr Guid ButtonPressed{0x25c81f56706c426cULL, 0xa6ea90564a92120cULL};
inline constexpr Guid ButtonReleased{0x25c81f56706c426cULL, 0xa6ea90564a92120dULL};
inline constexpr Guid ButtonHovered{0x25c81f56706c426cULL, 0xa6ea90564a92120eULL};
inline constexpr Guid ButtonUnhovered{0x25c81f56706c426cULL, 0xa6ea90564a92120fULL};
inline constexpr Guid SliderCaptureBegin{0x25c81f56706c426cULL, 0xa6ea90564a921210ULL};
inline constexpr Guid SliderCaptureEnd{0x25c81f56706c426cULL, 0xa6ea90564a921211ULL};
inline constexpr Guid SpinDragBegin{0x25c81f56706c426cULL, 0xa6ea90564a921212ULL};
inline constexpr Guid SpinDragEnd{0x25c81f56706c426cULL, 0xa6ea90564a921213ULL};
inline constexpr Guid SpinCommitted{0x25c81f56706c426cULL, 0xa6ea90564a921214ULL};
inline constexpr Guid SliderControllerBegin{0x25c81f56706c426cULL, 0xa6ea90564a921215ULL};
inline constexpr Guid SliderControllerEnd{0x25c81f56706c426cULL, 0xa6ea90564a921216ULL};

} // namespace rocks::ui::widgetEvents

namespace rocks::ui::widgetSignatures {

inline constexpr Guid NoArguments{0x8c27ce53c5ba4f09ULL, 0xaa2354c570858200ULL};
inline constexpr Guid Boolean{0x8c27ce53c5ba4f09ULL, 0xaa2354c570858201ULL};
// WidgetCheckState 인자다(21.5a절 — 옛 CheckState 인자 0x…202는 은퇴).
inline constexpr Guid CheckState{0x8c27ce53c5ba4f09ULL, 0xaa2354c570858209ULL};
inline constexpr Guid Integer{0x8c27ce53c5ba4f09ULL, 0xaa2354c570858203ULL};
// const String& 인자다(옛 StringView 인자 0x…204는 은퇴).
inline constexpr Guid String{0x8c27ce53c5ba4f09ULL, 0xaa2354c57085820aULL};
// (const String&, WidgetTextCommitReason)다(옛 0x…205는 은퇴).
inline constexpr Guid TextCommit{0x8c27ce53c5ba4f09ULL, 0xaa2354c57085820bULL};
// const InputChordValue& 인자다(옛 KeyChord 인자 0x…206은 은퇴).
inline constexpr Guid KeyChord{0x8c27ce53c5ba4f09ULL, 0xaa2354c57085820cULL};
inline constexpr Guid Number{0x8c27ce53c5ba4f09ULL, 0xaa2354c570858207ULL};
// (float, WidgetSpinCommitReason)다(옛 0x…208은 은퇴).
inline constexpr Guid SpinCommit{0x8c27ce53c5ba4f09ULL, 0xaa2354c57085820dULL};

} // namespace rocks::ui::widgetSignatures
