#pragma once

#include <Object/ObjectRegistry.h>
#include <UiWidget/WidgetAnimationTrack.h>

namespace rocks::ui {

// 여러 위젯의 불투명도와 렌더 변환 곡선을 담는 직렬화 가능한 애니메이션이다.
// 같은 정의를 여러 UserWidget에서 재생할 수 있다. 재생 시 곡선을 복사하므로 편집 중인
// 정의를 바꿔도 이미 실행 중인 인스턴스의 시간과 바인딩은 유지된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WidgetAnimation final : public Object {
    ROCKS_GENERATED_BODY()
public:
    explicit WidgetAnimation(Allocator& allocator) noexcept;
    Result<void> SetDuration(float seconds) noexcept;
    float Duration() const noexcept;
    Guid StableId() const noexcept;
    Result<void> SetTrack(Guid target, WidgetAnimationChannel channel,
                          const TangentCurve& curve) noexcept;
    Span<const WidgetAnimationTrack> Tracks() const noexcept;

private:
    friend class WidgetAsset;
    ROCKS_PROPERTY(ReadOnly, Name = "stableId") Guid stableId_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "duration") float duration_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "tracks") Array<WidgetAnimationTrack> tracks_;
};

} // namespace rocks::ui
