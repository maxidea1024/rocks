#include "SceneBoxes.h"

#include <UiRender/UiLaneRenderer.h>
#include <UiRender/UiScenePassContext.h>

namespace rocks::ui {

namespace {

template <class T>
Result<void> CopyInto(Array<T>& out, Span<const T> source) noexcept {
    if (auto sized = out.TryResize(source.Size()); !sized) {
        return sized;
    }

    for (usize index = 0; index < source.Size(); ++index) {
        out[index] = source[index];
    }

    return Result<void>::Success();
}

void RecordSceneBoxes(UiScenePassContext& scene, SceneBoxes& boxes) noexcept {
    UiFrameBatch batch;
    batch.vertices = boxes.vertices.View();
    batch.indices = boxes.indices.View();
    batch.commands = boxes.commands.View();
    // 실패하면 장면은 clear 색만 남는다(검사가 표식 색으로 잡는다).
    (void)scene.Renderer().AddBatchToScene(scene, batch);
}

} // namespace

Result<void> PostSceneBoxes(UiSceneExtract& extract, Allocator& allocator,
                            const PaintList& list) noexcept {
    DrawBatch batch(allocator);
    if (auto built = batch.Build(list); !built) {
        return built;
    }

    SceneBoxes boxes{Array<DrawVertex>(allocator), Array<uint32>(allocator),
                     Array<DrawCommand>(allocator)};
    auto copied = CopyInto(boxes.vertices, batch.Vertices());
    if (copied) {
        copied = CopyInto(boxes.indices, batch.Indices());
    }

    if (copied) {
        copied = CopyInto(boxes.commands, batch.Commands());
    }

    if (!copied) {
        return copied;
    }

    return extract.Post(Move(boxes), &RecordSceneBoxes);
}

} // namespace rocks::ui
