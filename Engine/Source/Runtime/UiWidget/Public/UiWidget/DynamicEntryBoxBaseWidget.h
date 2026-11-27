#pragma once

#include <UiWidget/PanelWidget.h>
#include <UiWidget/RadialBoxSettings.h>

namespace rocks::ui {

enum class DynamicBoxType : uint8 {
    Vertical,
    Horizontal,
    Wrap,
    VerticalWrap,
    Overlay,
    Radial
};

// 모든 엔트리를 유지하는 패널의 배치 방식을 고른다. 엔트리 수가 많아 가상화가 필요하면
// ListViewBaseWidget을 사용한다. 배치 방식을 바꾸려면 화면에서 떼고 ReleaseElements를 호출한다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API DynamicEntryBoxBaseWidget : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit DynamicEntryBoxBaseWidget(Allocator& allocator) noexcept;
    Result<void> SetBoxType(DynamicBoxType type) noexcept;
    DynamicBoxType BoxType() const noexcept;
    // 가로·세로 상자는 주축 간격, 줄바꿈 상자는 양 축 간격, 겹침 상자는 누적 오프셋으로 쓴다.
    Result<void> SetEntrySpacing(Vec2 spacing) noexcept;
    // 겹침 배치에서 연속된 엔트리 사이에 더할 오프셋을 순환한다. 빈 배열은 기본 간격을 쓴다.
    Result<void> SetSpacingPattern(Span<const Vec2> steps) noexcept;
    Span<const Vec2> GetSpacingPattern() const noexcept;
    Result<void> SetRadialSettings(const RadialBoxSettings& settings) noexcept;
    const RadialBoxSettings& GetRadialSettings() const noexcept;
    Vec2 EntrySpacing() const noexcept;
    Result<ObjectHandle> EntryAt(ObjectRegistry& objects, usize index) const noexcept;
    // 엔트리를 뒤에서부터 반납한다. 제거 실패 시 남은 엔트리는 패널과 풀의 소유권을 유지한다.
    Result<void> ResetEntries(ObjectRegistry& objects) noexcept;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "spacingPattern") Array<Vec2> spacingPattern_;
    ROCKS_PROPERTY(ReadOnly, Name = "radialSettings") RadialBoxSettings radialSettings_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "boxType") uint8 boxType_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "entrySpacing") Vec2 entrySpacing_ {};
};

} // namespace rocks::ui
