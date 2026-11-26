#pragma once

#include <UiFramework/Text/TextLabel.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 일반 본문, 서식 있는 본문, 편집 컨트롤이 공유하는 문단 배치 설정이다.
// 너비는 DPI 적용 전의 논리 단위다. 셰이핑이 끝난 뒤에도 줄바꿈과 가로 정렬을 바꿀 수 있다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API TextLayoutWidget : public Widget {
    ROCKS_GENERATED_BODY()
public:
    // 0이면 고정 너비 제한을 해제한다. 자동 줄바꿈과 함께 쓰면 더 좁은 폭을 따른다.
    Result<void> SetWrapWidth(float width) noexcept;
    float WrapWidth() const noexcept;
    // 부모가 할당한 너비가 바뀌면 새 너비에 맞춰 줄을 나눈다.
    Result<void> SetAutoWrapText(bool enabled) noexcept;
    bool AutoWrapText() const noexcept;
    // 긴 단어의 중간에서 줄바꿈을 허용할지 결정한다. 합성 문자 한 덩어리는 항상 한 줄에 남긴다.
    Result<void> SetWrapPolicy(text::TextWrapPolicy policy) noexcept;
    text::TextWrapPolicy WrapPolicy() const noexcept;
    // 정렬은 각 줄에 적용하며 셰이퍼가 정한 문단 방향과 글자 순서를 유지한다.
    Result<void> SetJustification(text::TextAlignment alignment) noexcept;
    text::TextAlignment Justification() const noexcept;

    // 왼쪽·위·오른쪽·아래 여백이다. 줄바꿈 너비와 정렬 기준은 여백 안쪽 공간을 따른다.
    Result<void> SetMargin(Vec4 margin) noexcept;
    Vec4 Margin() const noexcept;
    // 내용이 짧아져도 부모 측정에 제시할 최소 너비다. 줄을 나누는 폭은 바꾸지 않는다.
    Result<void> SetMinDesiredWidth(float width) noexcept;
    // 1은 셰이퍼의 기본 줄 높이다. 마지막 줄 옵션을 끄면 마지막 줄 아래에는 추가 간격을 두지
    // 않는다.
    Result<void> SetLineHeightPercentage(float scale) noexcept;
    Result<void> SetApplyLineHeightToBottomLine(bool enabled) noexcept;
    // 그림자는 기존 글리프 아틀라스와 투명도를 사용한다. 알파 0이면 그리기를 생략한다.
    Result<void> SetTextShadow(Vec2 offset, LinearColor color) noexcept;
    void SetTextDecoration(bool underline, bool strikeThrough) noexcept;
    // 한 줄 본문이나 고정 높이 상자에서 위젯 바깥으로 넘치는 글자를 자른다.
    void SetClipText(bool enabled) noexcept;

protected:
    // 편집 컨트롤처럼 루트가 글자 요소를 감싸는 경우 내부 레이블을 반환한다.
    virtual TextLabel& ResolveTextLabel(Element& element) const noexcept;
    Result<void> ApplyTextLayout(TextLabel& label) const noexcept;

private:
    Result<void> SetLayout(float width, text::TextAlignment alignment, bool automatic,
                           text::TextWrapPolicy policy) noexcept;

    Result<void> SetParagraph(const text::ParagraphMetrics& metrics) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "minDesiredWidth") float minDesiredWidth_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "margin") Vec4 margin_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "lineHeightPercentage") float lineHeightPercentage_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "applyLineHeightToBottomLine")
    bool applyLineHeightToBottomLine_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowOffset") Vec2 shadowOffset_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "shadowColor") LinearColor shadowColor_ { 0, 0, 0, 0 };
    ROCKS_PROPERTY(ReadOnly, Name = "underline") bool underline_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "strikeThrough") bool strikeThrough_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "clipText") bool clipText_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "wrapWidth") float wrapWidth_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "justification") uint8 justification_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "wrapPolicy") uint8 wrapPolicy_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "autoWrapText") bool autoWrapText_ = false;
};

} // namespace rocks::ui
