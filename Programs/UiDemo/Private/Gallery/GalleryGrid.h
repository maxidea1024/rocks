#pragma once

#include "GalleryParts.h"

#include <UiFramework/Layout/Grid.h>

// 위젯 탭의 행 격자다(UE SStarshipGallery::ConstructWidgetGallery의 NextSlot, 742–784행). 한 행은
// [활성 체크 상자][이름 — 부품 이름과 그 아래 쓰는 법]이고, 체크 상자를 끄면 그 행의
// 위젯 줄을 비활성으로 만든다(UE가 비활성 상태를 보여 주는 방법). RowsPerColumn 행마다 오른쪽 열
// 묶음으로 넘긴다(Docs/Design/UiToolkit 16.1절).
//
// 사용 예:
//   GalleryGrid grid(context);
//   const SharedPtr<Element> widgets[] = {button, other};
//   grid.AddRow("Primary Button", "Button + PrimaryButtonStyle()", widgets);
//   auto page = grid.Finish();
namespace rocks::ui::gallery {

class GalleryGrid {
public:
    // UE NextSlot의 RowCount·Cols다. 묶음(체크·이름·위젯 세 칸)은 가로로 MaxBands개까지만 둔다 —
    // 더 두면 창(1280) 오른쪽 밖으로 나가 세로 스크롤로 닿을 수 없다. 행이 많으면 묶음마다 행 수를
    // 늘린다(최소 RowsPerColumn). 그래서 칸 배치는 Finish에서 한다.
    static constexpr uint32 RowsPerColumn = 15;
    static constexpr uint32 ColumnsPerRow = 3;
    static constexpr uint32 MaxBands = 2;
    // 위젯 줄 안에서 위젯 사이 기본 간격이다.
    static constexpr float WidgetSpacing = 8;

    explicit GalleryGrid(GalleryContext& context) noexcept;

    // 위젯들을 가로로 놓은 행을 더한다. 첫 실패를 보존하고, 그 뒤 호출은 아무것도 하지 않는다.
    void AddRow(StringView name, StringView usage, Span<const SharedPtr<Element>> widgets,
                float spacing = WidgetSpacing) noexcept;
    void AddRow(StringView name, StringView usage, const SharedPtr<Element>& widget) noexcept;

    // 격자를 스크롤 영역에 담아 돌려준다. 행을 더하다 실패했으면 그 오류다.
    Result<SharedPtr<Element>> Finish() noexcept;

    usize RowCount() const noexcept;
    // 행의 위젯 줄과 활성 체크 상자다(self-test가 비활성 동작을 단언한다). 없으면 빈 참조다.
    SharedPtr<Element> RowContent(usize row) const noexcept;
    SharedPtr<Element> RowToggle(usize row) const noexcept;

private:
    Result<void> TryAddRow(StringView name, StringView usage,
                           Span<const SharedPtr<Element>> widgets, float spacing) noexcept;

    GalleryContext& context_;
    SharedPtr<Grid> grid_;
    Array<SharedPtr<Element>> contents_;
    Array<SharedPtr<Element>> toggles_;
    Array<SharedPtr<Element>> names_;
    uint32 count_ = 0;
    Result<void> status_ = Result<void>::Success();
};

} // namespace rocks::ui::gallery
