#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Color.h>
#include <Core/Math/Rect.h>
#include <UiCore/Geometry.h>
#include <UiCore/Brush.h>

namespace rocks::ui {

// 합성 시점이 중요한 효과는 일반 사각형과 다른 명령 경계를 갖는다.
enum class PaintOperation : uint8 {
    Draw,
    CopyBackdrop,
    BlurBackdrop,
    BlurToBuffer
};

// 창 좌표로 변환 가능한 사각형 하나다. 같은 layer에서는 추가 순서를 유지한다.
struct PaintBox {
    Geometry geometry;
    LinearColor color{1, 1, 1, 1};
    int32 layer = 0;
    bool hasClip = false;
    bool emptyClip = false;
    Box2<float> clip{};
    SlotHandle resource{};
    Vec2 uvStart{};
    Vec2 uvEnd{1, 1};
    PaintOperation operation = PaintOperation::Draw;
    float blurRadius = 0;
    int32 blurKernel = -1;
    Vec4 cornerRadius{};
    bool operator==(const PaintBox&) const noexcept = default;
};

// 줄 배치와 bearing을 반영한 글리프다. 좌표는 Element 로컬 단위, UV는 페이지 정규 좌표다.
struct PaintGlyph {
    Vec2 position;
    Vec2 size;
    SlotHandle resource;
    Box2<float> uv;
    // 단색 텍스트는 흰색을 쓴다. 최종 색은 스타일 색과 레이블 색의 곱이다.
    LinearColor tint{1, 1, 1, 1};
};

// Game lane에서 작성하는 CPU 그리기 목록이다. 현재 사각형만 기록하며 GPU 자원을 소유하지 않는다.
class ROCKS_UICORE_API PaintList {
public:
    explicit PaintList(Allocator& allocator) noexcept;

    // 할당에 실패하면 기존 목록을 유지한다.
    Result<void> TryAddBox(const PaintBox& box) noexcept;

    Result<void> TryAddGlyphRun(const Geometry& geometry, Span<const PaintGlyph> glyphs,
                                LinearColor color, int32 layer, bool useGlyphColor = true) noexcept;

    // 원래 크기로 만든 이미지나 9분할 브러시에서 로컬 사각형만 기록한다. 위젯 회전과 반전 UV를
    // 보존한다.
    Result<void> TryAddBrushRegion(const Geometry& geometry, const Brush& brush, LinearColor color,
                                   int32 layer, Box2<float> localRegion) noexcept;
    Result<void> TryAddBrush(const Geometry& geometry, const Brush& brush, LinearColor color,
                             int32 layer) noexcept;

    // 창 축에 평행한 clip만 지원한다. 회전된 clip은 Unsupported다.
    Result<void> PushClip(const Geometry& geometry) noexcept;
    void PopClip() noexcept;

    // 현재 그리는 위젯과 조상의 누적 불투명도다. 사용자 정의 효과의 강도 계산에도 쓴다.
    float InheritedOpacity() const noexcept;
    Span<const PaintBox> Boxes() const noexcept;
    // 이미 잘린 목록을 현재 clip과 교차시켜 원래 그리기 순서대로 붙인다.
    Result<void> TryAppendRecorded(Span<const PaintBox> boxes) noexcept;

    void Clear() noexcept;

private:
    friend class CacheRoot;
    friend class Element;
    float opacity_ = 1;
    struct ClipState {
        Box2<float> bounds;
        bool empty = false;
    };

    Array<PaintBox> boxes_;
    Array<ClipState> clips_;
};

} // namespace rocks::ui
