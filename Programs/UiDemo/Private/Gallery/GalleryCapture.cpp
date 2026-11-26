#include "GalleryCapture.h"

#include "../DemoShaders.h"

#include <AssetDatabase/AtomicFileWrite.h>
#include <Core/Text/StringBuilder.h>
#include <UiFramework/Application/Application.h>
#include <UiRender/DrawBatchBuffers.h>
#include <UiRender/WidgetSurfaceHost.h>

namespace rocks::ui::gallery {

namespace {

constexpr StringView CaptureDirectory = "Saved/UiDemo";
// RGBA 한 화소의 byte 수와 PPM이 쓰는 RGB byte 수다.
constexpr usize RgbaBytes = 4;
constexpr usize RgbBytes = 3;

// 지금 앞에 있는 탭을 그려 RGBA를 읽는다.
Result<void> Render(Allocator& allocator, graphics::Device& device,
                    graphics::PipelineHandle pipeline, SlotArray<BrushTexture>& textures,
                    Application& app, const SharedPtr<DockManager>& area, VectorImageAtlas& icons,
                    graphics::PipelineHandle imagePipeline, uint32 width, uint32 height,
                    uint64 frame, Array<uint8>& rgba) noexcept {
    const Vec2 size(static_cast<float>(width), static_cast<float>(height));
    if (auto arranged = app.PrepareLayout(Geometry::Root(size, {}), 1); !arranged) {
        return arranged;
    }

    WidgetSurfaceHost surfaces(allocator, device, ActiveShaders(), textures);
    surfaces.SetVectorImages(&icons, imagePipeline);
    auto prepared = surfaces.Prepare(area, frame, width, height, pipeline);
    if (!prepared) {
        return Result<void>::Failure(prepared.Error());
    }

    auto cache = app.UpdateArrangedPaintCache();
    if (!cache) {
        return Result<void>::Failure(cache.Error());
    }

    DrawBatch batch(allocator);
    DrawBatchBuffers buffers(device, ActiveShaders(), batch);
    if (auto updated = batch.Update(*cache.Value()); !updated) {
        return Result<void>::Failure(updated.Error());
    }

    if (auto uploaded = buffers.Upload(); !uploaded) {
        return uploaded;
    }

    if (auto synced = icons.Sync(imagePipeline); !synced) {
        return synced;
    }

    auto read =
        DrawBatchRenderer::ReadPixels(device, pipeline, buffers, textures, width, height, rgba);
    (void)icons.AdvanceFrame();
    return read;
}

// RGBA를 PPM(P6, RGB 행을 위에서 아래로)으로 쓴다.
Result<void> WritePpm(Allocator& allocator, StringView name, Span<const uint8> rgba, uint32 width,
                      uint32 height) noexcept {
    StringBuilder<> header(allocator);
    if (auto formatted = header.TryAppendFormat("P6\n{} {}\n255\n", width, height); !formatted) {
        return formatted;
    }

    const StringView prefix = header.View();
    Array<uint8> ppm(allocator);
    if (auto allocated =
            ppm.TryResize(prefix.ByteCount() + static_cast<usize>(width) * height * RgbBytes);
        !allocated) {
        return allocated;
    }

    usize out = 0;
    for (char byte : prefix) {
        ppm[out++] = static_cast<uint8>(byte);
    }

    for (usize i = 0; i + RgbaBytes <= rgba.Size(); i += RgbaBytes) {
        ppm[out++] = rgba[i];
        ppm[out++] = rgba[i + 1];
        ppm[out++] = rgba[i + 2];
    }

    auto saved = WriteFileAtomically(platform::PhysicalFileSystem(), allocator, CaptureDirectory,
                                     name, ppm.View());
    return saved ? Result<void>::Success() : Result<void>::Failure(saved.Error());
}

} // namespace

Result<void> CaptureGallery(Allocator& allocator, graphics::Device& device,
                            graphics::PipelineHandle pipeline, SlotArray<BrushTexture>& textures,
                            const SharedPtr<DockManager>& area, VectorImageAtlas& icons,
                            graphics::PipelineHandle imagePipeline, Span<const StringView> tabs,
                            uint32 width, uint32 height) noexcept {
    Application app(allocator);
    app.SetRoot(area);
    app.SetVectorImageResolver(&icons);
    uint64 frame = 0;
    Array<uint8> rgba(allocator);
    StringBuilder<> name(allocator);
    for (StringView tab : tabs) {
        if (auto activated = area->ActivateTab(tab); !activated) {
            return activated;
        }

        auto rendered = Render(allocator, device, pipeline, textures, app, area, icons,
                               imagePipeline, width, height, frame++, rgba);
        if (!rendered) {
            return rendered;
        }

        name.Clear();
        if (auto formatted = name.TryAppendFormat("Gallery-{}.ppm", tab); !formatted) {
            return formatted;
        }

        if (auto written = WritePpm(allocator, name.View(), rgba.View(), width, height); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui::gallery
