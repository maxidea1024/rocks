#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector4.h>

namespace rocks::text {

// 셰이퍼가 논리 순서로 넘긴 클러스터다. byteEnd는 UTF-8 원문 안의 끝 위치(미포함)다.
// 강제 줄바꿈은 CRLF도 한 항목으로 넘긴다. 공백의 레벨 초기화 여부는 경계 분석기가 제공한다.
struct TextCluster {
    usize byteEnd = 0;
    float advance = 0;
    uint8 bidiLevel = 0;
    bool breakAfter = false;
    bool hardBreak = false;
    bool resetAtLineEnd = false;
};

enum class TextAlignment : uint8 {
    Left,
    Center,
    Right
};

// 긴 단어가 한 줄의 폭을 넘을 때 자를 수 있는 경계를 정한다.
// Word는 셰이퍼가 허용한 줄바꿈 경계만 사용하고, WordOrCluster는 긴 단어도 클러스터 단위로 나눈다.
enum class TextWrapPolicy : uint8 {
    WordOrCluster,
    Word
};

// 문단 여백은 왼쪽·위·오른쪽·아래 순서이며 배치와 히트 테스트가 같은 좌표계를 쓴다.
struct ROCKS_TEXTLAYOUT_API ParagraphMetrics {
    Vec4 margin{};
    float lineHeightScale = 1;
    bool scaleLastLine = true;

    bool IsValid() const noexcept;
    bool operator==(const ParagraphMetrics&) const noexcept = default;
};

struct TextLine {
    usize byteBegin;
    usize byteEnd;
    usize firstVisual;
    usize visualCount;
    float x;
    float width;
    float y = 0;
    float height = 0;
};

struct VisualCluster {
    usize logicalIndex;
    float x;
    float advance;
    bool rightToLeft;
};

// 문자열 경계와 글리프 폭은 backend가 결정한다. 여기서는 줄별 시각 순서와 좌표를 계산한다.
class ROCKS_TEXTLAYOUT_API TextLayoutModel {
public:
    explicit TextLayoutModel(Allocator& allocator) noexcept;

    // wrapWidth가 0이면 명시적인 줄바꿈만 적용한다. 한 번의 호출은 같은 기본 방향의 단락들이다.
    // alignmentWidth로 줄 정렬에 쓸 공간을 지정한다. 0이면 지정 폭 또는 가장 긴 줄을 쓴다.
    Result<void> Layout(Span<const TextCluster> clusters, float wrapWidth, float lineHeight,
                        TextAlignment alignment, bool rightToLeft, float alignmentWidth = 0,
                        TextWrapPolicy policy = TextWrapPolicy::WordOrCluster,
                        const ParagraphMetrics& metrics = {}) noexcept;

    Vec2 DesiredSize() const noexcept;
    Span<const TextLine> Lines() const noexcept;
    Span<const VisualCluster> VisualClusters() const noexcept;

    // 줄 밖의 점은 가장 가까운 줄과 클러스터 끝으로 제한한다.
    usize HitTest(Vec2 local) const noexcept;
    // 줄 경계에서 trailing이면 이전 줄, 아니면 다음 줄의 시작 위치를 돌려준다.
    Result<Vec2> CaretPosition(usize byteOffset, bool trailing) const noexcept;

private:
    Allocator* allocator_;
    Array<usize> ends_;
    Array<TextLine> lines_;
    Array<VisualCluster> visual_;
    Vec2 size_{};
    float lineHeight_ = 0;
    bool rightToLeft_ = false;
};

} // namespace rocks::text
