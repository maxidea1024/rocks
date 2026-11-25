#include "WorkspaceCapture.h"
#include <AssetDatabase/AtomicFileWrite.h>
#include <Core/Text/StringBuilder.h>
#include <UiFramework/Application.h>
#include <UiRender/DrawBatchBuffers.h>
#include <UiRender/WidgetSurfaceHost.h>

namespace rocks::ui {

Result<void> CaptureWorkspace(Allocator& allocator, graphics::Device& device,
                              graphics::PipelineHandle pipeline, SlotArray<BrushTexture>& textures,
                              const SharedPtr<DockManager>& root, uint32 width,
                              uint32 height) noexcept {
    Application app(allocator);
    app.SetRoot(root);
    const Vec2 size(static_cast<float>(width), static_cast<float>(height));
    auto arranged = app.PrepareLayout(Geometry::Root(size, {}), 1);
    if (!arranged) {
        return arranged;
    }
    WidgetSurfaceHost surfaces(allocator, device, textures);
    auto preparedSurfaces = surfaces.Prepare(root, 0, width, height, pipeline);
    if (!preparedSurfaces) {
        return Result<void>::Failure(preparedSurfaces.Error());
    }
    auto cache = app.UpdateArrangedPaintCache();
    if (!cache) {
        return Result<void>::Failure(cache.Error());
    }
    DrawBatch batch(allocator);
    DrawBatchBuffers buffers(device, batch);
    auto prepared = batch.Update(*cache.Value());
    if (!prepared) {
        return Result<void>::Failure(prepared.Error());
    }
    auto uploaded = buffers.Upload();
    if (!uploaded) {
        return uploaded;
    }

    Array<uint8> rgba(allocator);
    auto read =
        DrawBatchRenderer::ReadPixels(device, pipeline, buffers, textures, width, height, rgba);
    if (!read) {
        return read;
    }

    // PPM은 파일 머리 다음에 RGB 순서의 행을 위에서 아래로 기록한다.
    StringBuilder<> header(allocator);
    auto formatted = header.TryAppendFormat("P6\n{} {}\n255\n", width, height);
    if (!formatted) {
        return formatted;
    }
    const auto prefix = header.View();
    Array<uint8> ppm(allocator);
    auto allocated = ppm.TryResize(prefix.ByteCount() + static_cast<usize>(width) * height * 3);
    if (!allocated) {
        return allocated;
    }
    usize out = 0;
    for (char byte : prefix) {
        ppm[out++] = static_cast<uint8>(byte);
    }
    for (usize i = 0; i < rgba.Size(); i += 4) {
        ppm[out++] = rgba[i];
        ppm[out++] = rgba[i + 1];
        ppm[out++] = rgba[i + 2];
    }
    auto saved = WriteFileAtomically(platform::PhysicalFileSystem(), allocator, "Saved/UiDemo",
                                     "Orbit.ppm", ppm.View());
    return saved ? Result<void>::Success() : Result<void>::Failure(saved.Error());
}

} // namespace rocks::ui
