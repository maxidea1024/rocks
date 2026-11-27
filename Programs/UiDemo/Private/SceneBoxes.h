#pragma once

#include <UiCore/DrawBatch.h>
#include <UiCore/Rendering/UiSceneExtract.h>

namespace rocks::ui {

// 자체 시험용 장면 packet이다: 단색 상자(UI 그리기 묶음)를 장면 대상에 UI 셰이더로
// 그린다(Docs/Design/UiToolkit 18.3절 — UiLaneRenderer::AddBatchToScene). 배열은 packet이
// 소유하므로 게임 lane 주소가 없다.
struct SceneBoxes {
    Array<DrawVertex> vertices;
    Array<uint32> indices;
    Array<DrawCommand> commands;
};

// list를 묶어 장면 packet으로 게시한다. 실패는 그 오류다.
Result<void> PostSceneBoxes(UiSceneExtract& extract, Allocator& allocator,
                            const PaintList& list) noexcept;

} // namespace rocks::ui

ROCKS_LANE_TRANSFERABLE(rocks::ui::SceneBoxes);
