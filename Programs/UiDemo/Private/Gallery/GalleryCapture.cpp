#include "GalleryCapture.h"

#include "../DemoCapture.h"

#include <AssetDatabase/AtomicFileWrite.h>
#include <Core/ScopeExit.h>
#include <Core/Text/StringBuilder.h>
#include <UiCore/ManualClock.h>
#include <UiCore/Rendering/SurfaceRecorder.h>
#include <UiFramework/Application/Application.h>

namespace rocks::ui::gallery {

namespace {

constexpr StringView CaptureDirectory = "Saved/UiDemo";
// RGBA 한 화소의 byte 수와 PPM이 쓰는 RGB byte 수다.
constexpr usize RgbaBytes = 4;
constexpr usize RgbBytes = 3;
// 캡처 바탕색이다(내용이 덮지 않는 틈 — 이전 직접 읽기 경로와 같은 색).
constexpr LinearColor GalleryCaptureClear{0.025f, 0.03f, 0.04f, 1};
// 캡처 한 장마다 올리는 수동 시계 간격(초)이다(60 Hz 한 프레임 — 20.3절 결정성).
constexpr double CaptureFrameSeconds = 1.0 / 60.0;

// 지금 앞에 있는 탭을 그려 RGBA를 읽는다.
Result<void> Render(Allocator& allocator, UiRenderer& renderer, UiTextureId target,
                    Application& app, const SharedPtr<DockManager>& area, VectorImageAtlas& icons,
                    uint32 width, uint32 height, uint64 frame, Array<uint8>& rgba) noexcept {
    const Vec2 size(static_cast<float>(width), static_cast<float>(height));
    if (auto arranged = app.PrepareLayout(Geometry::Root(size, {}), 1); !arranged) {
        return arranged;
    }

    // 탭마다 새 표면 기록기로 그린다(이전 경로처럼 보관 표면을 매번 처음부터 채운다).
    SurfaceRecorder surfaces(allocator, renderer);
    surfaces.SetVectorImages(&icons);
    UiDrawBuffer buffer(allocator);
    auto prepared = surfaces.Record(area, frame, width, height, buffer);
    if (!prepared) {
        return Result<void>::Failure(prepared.Error());
    }

    auto cache = app.UpdateArrangedPaintCache();
    if (!cache) {
        return Result<void>::Failure(cache.Error());
    }

    DrawBatch batch(allocator);
    if (auto updated = batch.Update(*cache.Value()); !updated) {
        return Result<void>::Failure(updated.Error());
    }

    if (auto uploaded = icons.Upload(); !uploaded) {
        return uploaded;
    }

    auto read =
        DrawAndCapture(renderer, buffer, target, width, height, GalleryCaptureClear, batch, rgba);
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

Result<void> CaptureGallery(Allocator& allocator, UiRenderer& renderer,
                            const SharedPtr<DockManager>& area, VectorImageAtlas& icons,
                            Span<const StringView> tabs, uint32 width, uint32 height) noexcept {
    auto target = renderer.CreateRenderTarget(width, height, UiTextureSampling::Image);
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    ROCKS_ON_SCOPE_EXIT {
        renderer.ReleaseTexture(target.Value());
    };
    // 애니메이션이 실행마다 같은 위상에서 찍히도록 수동 시계를 쓴다(Docs/Design/UiToolkit 20.3절).
    ManualClock clock;
    Application app(allocator);
    app.SetClock(clock.Function());
    app.SetRoot(area);
    app.SetVectorImageResolver(&icons);
    uint64 frame = 0;
    Array<uint8> rgba(allocator);
    StringBuilder<> name(allocator);
    for (StringView tab : tabs) {
        if (auto activated = area->ActivateTab(tab); !activated) {
            return activated;
        }

        clock.Advance(CaptureFrameSeconds);
        app.BeginFrame();
        auto rendered = Render(allocator, renderer, target.Value(), app, area, icons, width, height,
                               frame++, rgba);
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
