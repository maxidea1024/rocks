#pragma once

#include <Localization/Text.h>
#include <UiFramework/Text/TextLabel.h>

namespace rocks::ui {

// 화면의 언어·글꼴·아틀라스에 맞춰 글자를 셰이핑한다(Docs/Design/UiToolkit 21.2절). 셰이핑이 어느
// 글꼴·아틀라스에 묶이는지는 호스트가 정하므로, Element(편집 텍스트)와 Widget(WidgetTree)이 같은
// 인터페이스를 받는다. 구현은 Text를 현재 로캘로 해석한 뒤 TextLabel::SetShapedText로 클러스터와
// 글리프를 한 번에 넘긴다. 실패한 호출은 레이블에 이전 문자열과 글리프를 함께 남겨야 한다.
//
// 사용 예:
//   class GameShaper final : public TextShaper { ... };
//   tree.SetTextShaper(shaper);
class ROCKS_UIFRAMEWORK_API TextShaper {
public:
    virtual ~TextShaper() noexcept = default;
    virtual Result<void> Shape(TextLabel& label, const Text& text, float fontSize) noexcept = 0;
    // 말줄임표도 본문과 같은 글꼴·크기·아틀라스에서 얻는다. 한 줄·폭 있는 "…"가 아니면
    // InvalidArgument다.
    Result<SharedPtr<TextLabel>> ShapeEllipsis(Allocator& allocator, float fontSize) noexcept;
    // utf8의 단어 경계 byte 위치를 out에 쓴다(0은 빼고 끝은 넣는다, 오름차순 — text::TextBackend
    // FindBoundaries(Word)와 같은 모양, Docs/Design/UiToolkit 22.4절). 편집 텍스트의 더블클릭 단어
    // 선택이 쓴다. 기본 구현은 문자 부류(공백·문장부호·그 밖)가 바뀌는 곳과 문장부호 하나하나에서
    // 나눈다 — 글꼴 백엔드(ICU)가 있는 호스트는 재정의해 UE FTextBreakIterator와 같은 단어 경계를
    // 준다. 잘못된 UTF-8은 InvalidArgument, 할당 실패는 그 오류다.
    virtual Result<void> FindWordBoundaries(StringView utf8, Array<usize>& out) noexcept;
};

} // namespace rocks::ui
