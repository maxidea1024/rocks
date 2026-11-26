#pragma once

#include <Core/Text/String.h>
#include <TextLayout/TextLayoutModel.h>
#include <UiCore/ColorRef.h>
#include <UiCore/Element.h>

namespace rocks::ui {

// 한 클러스터에 여러 글리프가 있거나 글리프가 없는 경우를 모두 허용한다.
// position은 클러스터의 시각적 왼쪽과 줄 위쪽을 기준으로 한 글리프 위치다.
struct LabelGlyph {
    usize clusterIndex;
    PaintGlyph glyph;
};

// 논리 클러스터에 연결된 장식이다. 글리프가 없는 공백도 선의 길이에 포함한다.
struct LabelDecoration {
    LinearColor color{1, 1, 1, 1};
    bool underline = false;
    bool strikeThrough = false;
    bool operator==(const LabelDecoration&) const noexcept = default;
};

class ROCKS_UIFRAMEWORK_API TextLabel : public Element {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    explicit TextLabel(Allocator& allocator) noexcept;

    // clusters와 glyphs는 같은 문자열을 셰이핑한 결과다. glyphs는 논리 클러스터 순이어야 한다.
    // 원문과 결과를 함께 교체하므로 중간에 할당에 실패해도 이전 텍스트를 계속 그린다.
    Result<void> SetShapedText(StringView source, Span<const text::TextCluster> clusters,
                               Span<const LabelGlyph> glyphs, float lineHeight, bool rightToLeft,
                               Span<const LabelDecoration> decorations = {}) noexcept;
    Result<void> SetWrapping(float width, text::TextAlignment alignment) noexcept;
    // 자동 줄바꿈을 켜면 부모가 할당한 너비와 지정 너비 중 작은 값에서 줄을 나눈다.
    // 지정 너비가 0이면 할당된 너비만 사용한다. 바뀐 줄에 기존 셰이핑 결과를 재사용한다.
    Result<void>
    SetLayoutOptions(float width, text::TextAlignment alignment, bool autoWrap,
                     text::TextWrapPolicy policy = text::TextWrapPolicy::WordOrCluster) noexcept;
    // 편집 컨트롤은 자식의 최종 위치를 정하기 전 이 폭으로 줄을 배치해 커서 좌표를 얻는다.
    Result<void> ArrangeForWidth(float width) noexcept;
    Vec2 MeasureForWidth(float width) noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> SetParagraphStyle(const text::ParagraphMetrics& metrics, Vec2 shadowOffset,
                                   LinearColor shadowColor) noexcept;
    void SetDecoration(bool underline, bool strikeThrough) noexcept;
    float LineStride() const noexcept;
    Result<void> SetMinDesiredWidth(float width) noexcept;
    Result<void> CopyParagraphTo(TextLabel& target) const noexcept;
    // 부모가 허용한 폭과 높이를 넘는 줄에서만 사용한다. 표시는 전용 셰이핑 결과를 공유한다.
    void SetEllipsisMarker(SharedPtr<const TextLabel> marker) noexcept;
    // 글자 색이다(UE STextBlock ColorAndOpacity). 기본은 부모 전경색
    // 상속(ColorSource::Foreground)이라 버튼 안의 레이블은 버튼 상태 전경색을 따른다. LinearColor는
    // 지정 색(ColorRef::Specified)이다.
    void SetColor(LinearColor color) noexcept;
    void SetColor(ColorRef color) noexcept;
    ColorRef GetColor() const noexcept;
    StringView GetText() const noexcept;
    StringView DefaultAccessibleText() const noexcept override;
    // 편집 컨트롤은 셰이퍼가 확정한 클러스터 경계와 실제 줄 위치를 함께 사용한다.
    const text::TextLayoutModel& Layout() const noexcept;
    // 셰이핑한 논리 순서의 원본이다. 리치 텍스트는 이 경계를 유지하면서 클러스터별 스타일을
    // 입힌다. 반환된 뷰는 다음 SetShapedText 호출까지 유효하다.
    Span<const text::TextCluster> ShapedClusters() const noexcept;
    Span<const LabelGlyph> ShapedGlyphs() const noexcept;
    float LineHeight() const noexcept;
    bool IsRightToLeft() const noexcept;

    usize HitText(Vec2 localPosition) const noexcept;
    Result<Vec2> CaretPosition(usize byteOffset, bool trailing) const noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Result<void> Reflow(Span<const text::TextCluster> clusters, Span<const LabelGlyph> glyphs,
                        float width, float lineHeight, text::TextAlignment alignment,
                        bool rightToLeft, float alignmentWidth, text::TextWrapPolicy policy,
                        const text::ParagraphMetrics& metrics, Vec2 shadowExtent) noexcept;

    Result<void> PaintOverflow(PaintList& list, int32 layer) const noexcept;
    Result<void> PaintDecorations(PaintList& list, const Geometry& area, int32 layer,
                                  const text::TextLine& line, usize first,
                                  usize count) const noexcept;

    Allocator& allocator_;
    String source_;
    Array<text::TextCluster> clusters_;
    Array<LabelGlyph> glyphs_;
    text::TextLayoutModel layout_;
    Array<PaintGlyph> positioned_;
    Array<usize> visualGlyphStarts_;
    SharedPtr<const TextLabel> ellipsisMarker_;
    Array<LabelDecoration> decorations_;
    text::ParagraphMetrics metrics_{};
    Vec2 shadowOffset_{};
    LinearColor shadowColor_{0, 0, 0, 0};
    bool underline_ = false;
    bool strikeThrough_ = false;
    ColorRef color_{ColorSource::Foreground};
    text::TextWrapPolicy policy_ = text::TextWrapPolicy::WordOrCluster;
    float minDesiredWidth_ = 0;
    float wrapWidth_ = 0;
    float allottedWidth_ = 0;
    bool autoWrap_ = false;
    float lineHeight_ = 0;
    text::TextAlignment alignment_ = text::TextAlignment::Left;
    bool rightToLeft_ = false;
};

} // namespace rocks::ui
