#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Color.h>
#include <Core/Math/Rect.h>
#include <UiCore/VectorImageResolver.h>
#include <UiCore/Geometry.h>
#include <UiCore/Brush.h>
#include <UiCore/ColorRef.h>

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
    // 블러는 모서리 반경, 둥근 상자는 모서리 반경(왼쪽 위부터 시계 방향)이다.
    Vec4 cornerRadius{};
    // 둥근 상자다(UW3a). roundedSize는 잘라내기 전 상자 전체의 로컬 크기, UV는 그 안의 0..1이다.
    bool roundedBox = false;
    Vec2 roundedSize{};
    float outlineWidth = 0.0f;
    LinearColor outlineColor{0, 0, 0, 0};
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

// UE 5.8 비활성 효과의 알파 배율이다(SlateElementPixelShader.usf 645행 OutColor.a *= .45f).
inline constexpr float DisabledEffectOpacity = 0.45f;

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

    // 위젯이 스타일 색(ColorRef)을 풀 테마다(UW3b). nullptr이면 DefaultThemeColors()다. 목록은
    // 테마를 빌리므로 소유자(ui::Application)가 목록보다 오래 살아야 한다. Clear는 테마를 유지한다.
    // revision은 같은 테마 객체의 값이 바뀔 때마다 늘린다(지역 캐시가 주소만으로는 바뀐 것을
    // 모른다).
    void SetTheme(const ThemeColors* theme, uint64 revision = 0) noexcept;
    const ThemeColors& Theme() const noexcept;

    // 벡터 brush를 래스터 자리로 바꿀 resolver다(Docs/Design/UiToolkit 17.2절). nullptr이면 벡터
    // brush는 아무것도 그리지 않는다(창·GPU 없는 시험 — UE null renderer와 같다). 목록은 resolver를
    // 빌리므로 소유자가 목록보다 오래 살아야 한다. Clear는 resolver를 유지한다.
    void SetVectorImageResolver(VectorImageResolver* resolver) noexcept;
    VectorImageResolver* GetVectorImageResolver() const noexcept;
    uint64 ThemeRevision() const noexcept;

    // 지금 그리는 요소가 물려받은 전경색이다(UE FWidgetStyle::GetForegroundColor). 루트는 흰색이다.
    // 텍스트처럼 ColorSource::Foreground 색을 쓰는 요소는 ColorContext()로 푼다.
    LinearColor ForegroundColor() const noexcept;
    StyleColorContext ColorContext() const noexcept;

    // 지금 그리는 요소나 조상이 비활성인지다. 비활성 하위 트리는 UE 5.8 비활성 효과처럼 알파에
    // DisabledEffectOpacity를 곱해 그린다(SlateElementPixelShader.usf 645행 — 레거시 탈색은 UE도
    // 끈다).
    bool IsDisabledEffect() const noexcept;
    // 비활성 효과 예외와 무관하게, 지금 그리는 요소나 조상이 비활성인지다(UE ShouldBeEnabled의
    // 반대).
    bool IsDisabled() const noexcept;

    // 비활성 전용 brush를 그리는 요소(UE SButton의 bShowDisabledEffect가 false인 경우)가 자기
    // 그림에서만 비활성 효과를 뺀다. 범위가 끝나면 되돌린다. 자식에는 효과가 그대로 간다.
    class DisabledEffectExemption {
    public:
        explicit DisabledEffectExemption(PaintList& list) noexcept
            : list_(list), previous_(list.disabledExempt_) {
            list_.disabledExempt_ = true;
        }

        ~DisabledEffectExemption() noexcept {
            list_.disabledExempt_ = previous_;
        }

        DisabledEffectExemption(const DisabledEffectExemption&) = delete;
        DisabledEffectExemption& operator=(const DisabledEffectExemption&) = delete;

    private:
        PaintList& list_;
        bool previous_;
    };

    void Clear() noexcept;

private:
    friend class CacheRoot;
    friend class Element;
    float opacity_ = 1;
    const ThemeColors* theme_ = nullptr;
    VectorImageResolver* vectorImages_ = nullptr;
    uint64 themeRevision_ = 0;
    LinearColor foreground_{1, 1, 1, 1};
    bool disabled_ = false;
    bool disabledExempt_ = false;
    struct ClipState {
        Box2<float> bounds;
        bool empty = false;
    };

    Array<PaintBox> boxes_;
    Array<ClipState> clips_;
};

} // namespace rocks::ui
