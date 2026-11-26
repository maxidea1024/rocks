#pragma once

#include <Localization/Text.h>
#include <UiFramework/Text/TextLabel.h>

namespace rocks::ui {

// 게임 화면의 언어·글꼴·아틀라스에 맞춰 텍스트를 셰이핑한다. 화면을 열기 전 WidgetTree에 연결한다.
// 구현은 Text를 현재 로캘로 해석한 뒤 TextLabel::SetShapedText로 클러스터와 글리프를 한 번에
// 넘긴다. 실패한 호출은 레이블에 이전 문자열과 글리프를 함께 남겨야 한다.
// 위젯이 스스로 만드는 글자(캡션·접근성 이름·탭 글자)의 기본 글꼴 크기다.
inline constexpr float DefaultWidgetFontSize = 16.0f;

class ROCKS_UIWIDGET_API WidgetTextShaper {
public:
    // 말줄임표도 본문과 같은 폰트·크기·아틀라스에서 얻는다.
    Result<SharedPtr<TextLabel>> ShapeEllipsis(Allocator& allocator, float fontSize) noexcept;
    virtual ~WidgetTextShaper() noexcept = default;
    virtual Result<void> Shape(TextLabel& label, const Text& text, float fontSize) noexcept = 0;
};

} // namespace rocks::ui
