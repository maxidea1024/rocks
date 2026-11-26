#include "DemoLog.h"
#include "DemoShaders.h"
#include "DockWindows.h"

#include <Core/Diagnostics/Log.h>

namespace rocks::ui {

namespace {

Result<IntVec2> DesktopPoint(const platform::Window& window, Vec2 point) noexcept {
    IntVec2 client;
    if (!math::TryTruncToInt32(point.x, client.x) || !math::TryTruncToInt32(point.y, client.y)) {
        return Result<IntVec2>::Failure(ErrorCode::InvalidArgument);
    }

    return window.ClientToDesktop(client);
}

Result<IntVec2> PreviewSize(Vec2 size) noexcept {
    IntVec2 pixels;
    if (!math::TryTruncToInt32(math::Ceil(size.x), pixels.x) ||
        !math::TryTruncToInt32(math::Ceil(size.y), pixels.y) || pixels.x <= 0 || pixels.y <= 0) {
        return Result<IntVec2>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<IntVec2>::Success(pixels);
}

} // namespace

DockWindows::Surface::Surface(Allocator& allocator, graphics::Device& graphicsDevice,
                              UniquePtr<platform::Window> native,
                              const SharedPtr<DockManager>& manager,
                              SlotArray<BrushTexture>& textures) noexcept
    : device(graphicsDevice), window(Move(native)), area(manager), app(allocator), adapter(app),
      queue(allocator), consumed(allocator), batch(allocator),
      buffers(device, ActiveShaders(), batch),
      surfaces(allocator, device, ActiveShaders(), textures) {
    app.SetRoot(area);
    window->SetRegionHitTest(
        platform::Window::RegionHitTest::CreateLambda([this](IntVec2 point) noexcept {
            return app.QueryWindowRegion(
                {static_cast<float>(point.x), static_cast<float>(point.y)});
        }));
}

DockWindows::Surface::~Surface() noexcept {
    // 창이 멤버 중 거의 마지막에 파괴되므로 그 전에 이 호스트를 가리키는 콜백을 먼저 끊는다.
    window->SetLiveResizeHandler({});
    window->SetRegionHitTest({});
    area->SetOnExternalDrop({});
    area->SetWindowHitTest({});
    if (!chain.IsNull()) {
        device.Retire(chain);
    }
}

DockWindows::DockWindows(Allocator& allocator, graphics::Device& device,
                         graphics::PipelineHandle pipeline, SlotArray<BrushTexture>& textures,
                         const SharedPtr<CommandList>& commands, VectorImageAtlas& icons,
                         graphics::PipelineHandle imagePipeline) noexcept
    : allocator_(allocator), device_(device), pipeline_(pipeline), textures_(textures),
      commands_(commands), icons_(icons), imagePipeline_(imagePipeline), windows_(allocator) {
}

Result<SlotHandle> DockWindows::Add(UniquePtr<platform::Window> window,
                                    const SharedPtr<DockManager>& area) noexcept {
    auto surface =
        UniquePtr<Surface>::TryMake(allocator_, allocator_, device_, Move(window), area, textures_);
    if (!surface) {
        return Result<SlotHandle>::Failure(surface.Error());
    }

    auto& host = *surface.Value();
    host.app.SetVectorImageResolver(&icons_);
    host.surfaces.SetVectorImages(&icons_, imagePipeline_);
    auto initialized = host.app.SetCommands({}, commands_);
    if (initialized) {
        initialized = host.queue.TryInitialize(1024);
    }

    if (!initialized) {
        return Result<SlotHandle>::Failure(initialized.Error());
    }

    auto native = host.window->NativeHandle();
    graphics::SwapChainDesc desc;
    desc.surface.window = native.window;
    desc.surface.instance = native.instance;
    desc.width = 1u > host.window->Width() ? 1u : host.window->Width();
    desc.height = 1u > host.window->Height() ? 1u : host.window->Height();
    auto created = device_.CreateSwapChain(desc);
    if (!created) {
        return Result<SlotHandle>::Failure(created.Error());
    }

    host.chain = created.Value();
    // 창 테두리를 끄는 동안 호스트 루프가 멈추므로 창이 이 콜백으로 그 창만 다시 그리게 한다.
    host.window->SetLiveResizeHandler(
        platform::Window::LiveResizeHandler::CreateLambda([this, source = &host]() noexcept {
            if (source->liveResizeError != ErrorCode::None) {
                return;
            }

            auto rendered = Render(*source, false);
            if (!rendered) {
                source->liveResizeError = rendered.Error();
            }
        }));
    auto slot = windows_.TryAdd(Move(surface.Value()));
    if (!slot) {
        return slot;
    }

    area->SetWindowHitTest(
        DockWindowHitTest::CreateLambda([this, source = &host](Vec2 point, Vec2& client) noexcept {
            return FindArea(*source, point, client);
        }));
    area->SetOnExternalDrop(Delegate<void(StringView, Vec2)>::CreateLambda(
        [this, source = &host](StringView id, Vec2 p) noexcept {
            // 대상 창이나 스왑 체인을 만들지 못하면 패널은 원래 창에 남는다.
            (void)FloatTab(*source, id, p);
        }));
    return slot;
}

SharedPtr<DockManager> DockWindows::FindArea(Surface& source, Vec2 point,
                                             Vec2& clientPoint) noexcept {
    auto desktop = DesktopPoint(*source.window, point);
    if (!desktop) {
        return {};
    }

    for (const auto& other : windows_) {
        if (other->window->IsCloseRequested() ||
            !other->window->IsAtDesktopPoint(desktop.Value())) {
            continue;
        }

        auto local = other->window->DesktopToClient(desktop.Value());
        if (!local) {
            return {};
        }

        const Vec2 position(static_cast<float>(local.Value().x),
                            static_cast<float>(local.Value().y));
        if (other->area->GetGeometry().Contains(position)) {
            clientPoint = position;
            return other->area;
        }
    }

    return {};
}

Result<void> DockWindows::FloatTab(Surface& source, StringView id, Vec2) noexcept {
    FloatingTabPreview preview;
    if (!source.area->GetFloatingPreview(preview)) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto desktop = DesktopPoint(*source.window, preview.origin);
    auto size = PreviewSize(preview.size);
    if (!desktop || !size) {
        return Result<void>::Failure(!desktop ? desktop.Error() : size.Error());
    }

    platform::WindowDesc desc;
    desc.title = id;
    desc.width = static_cast<uint32>(size.Value().x);
    desc.height = static_cast<uint32>(size.Value().y);
    desc.positionClientOrigin = true;
    desc.useDesktopPosition = true;
    desc.desktopPosition = desktop.Value();
    auto window = platform::Window::Create(allocator_, desc);
    if (!window) {
        return Result<void>::Failure(window.Error());
    }

    auto area = SharedPtr<DockManager>::TryMake(allocator_, allocator_);
    if (!area) {
        return Result<void>::Failure(area.Error());
    }

    auto added = Add(Move(window.Value()), area.Value());
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    auto moved = source.area->TransferTabTo(id, *area.Value());
    if (!moved) {
        (void)windows_.TryRemove(added.Value());
    }

    return moved;
}

Result<void> DockWindows::UpdateDragImage(Surface& host) noexcept {
    FloatingTabPreview preview;
    if (!host.area->GetFloatingPreview(preview)) {
        host.dragImage.Reset();
        host.previewGesture = 0;
        return Result<void>::Success();
    }

    auto position = DesktopPoint(*host.window, preview.origin);
    if (!position) {
        return Result<void>::Failure(position.Error());
    }

    if (host.previewGesture == preview.gesture) {
        if (host.dragImage && host.previewPosition != position.Value()) {
            host.dragImage->MoveTo(position.Value());
            host.previewPosition = position.Value();
        }

        return Result<void>::Success();
    }

    // 처음 창 밖으로 나온 시점에만 픽셀을 읽는다. 이후에는 네이티브 창의 위치만 바꾼다.
    host.dragImage.Reset();
    host.previewGesture = preview.gesture;
    auto size = PreviewSize(preview.size);
    if (!size) {
        return Result<void>::Failure(size.Error());
    }

    PaintList list(allocator_);
    list.SetVectorImageResolver(&icons_);
    auto result = host.area->PaintFloatingPreview(list);
    if (!result) {
        return result;
    }

    DrawBatch batch(allocator_);
    result = batch.Build(list);
    if (!result) {
        return result;
    }

    DrawBatchBuffers buffers(device_, ActiveShaders(), batch);
    result = buffers.Upload();
    if (!result) {
        return result;
    }

    // 미리보기가 받은 SVG 자리의 페이지를 읽기 전에 올린다(17.2절 프레임 순서).
    result = icons_.Sync(imagePipeline_);
    if (!result) {
        return result;
    }

    Array<uint8> pixels(allocator_);
    result = DrawBatchRenderer::ReadPixels(device_, pipeline_, buffers, textures_,
                                           static_cast<uint32>(size.Value().x),
                                           static_cast<uint32>(size.Value().y), pixels);
    if (!result) {
        return result;
    }

    auto image = platform::DragImage::Create(allocator_, *host.window, position.Value(),
                                             size.Value(), pixels.View(), 170);
    if (!image) {
        return Result<void>::Failure(image.Error());
    }

    host.dragImage = Move(image.Value());
    host.previewPosition = position.Value();
    return Result<void>::Success();
}

Result<bool> DockWindows::Update(Surface& host) noexcept {
    host.window->PumpEvents(host.queue);
    auto sized = host.consumed.TryResize(host.queue.Events().Size());
    if (!sized) {
        return Result<bool>::Failure(sized.Error());
    }

    const bool hadInput = !host.queue.Events().IsEmpty();
    auto routed = host.adapter.RouteBatch(host.queue.Events(), host.consumed.View(),
                                          host.queue.DroppedCount());
    host.queue.Clear();
    // 입력 처리 오류는 UI 버그다. 앱을 닫지 않고 남겨 원인을 찾게 한다(넘친 큐는 정상 상황).
    if (!routed && routed.Error() != ErrorCode::CapacityExceeded) {
        Log::Error(LogUiDemo, "Input routing failed: {Error}", ErrorCodeName(routed.Error()));
    }

    if (host.liveResizeError != ErrorCode::None) {
        return Result<bool>::Failure(host.liveResizeError);
    }

    return Render(host, hadInput || !routed);
}

Result<bool> DockWindows::Render(Surface& host, bool refreshCursor) noexcept {
    uint32 width = 0, height = 0;
    bool repaint = host.window->ConsumeRepaint();
    if (host.window->ConsumeResize(width, height) && width && height) {
        auto resized = device_.ResizeSwapChain(host.chain, width, height);
        if (!resized) {
            return Result<bool>::Failure(resized.Error());
        }

        repaint = true;
    }

    if (host.window->IsMinimized() || !host.window->Width() || !host.window->Height() ||
        host.window->IsCloseRequested()) {
        host.dragImage.Reset();
        host.previewGesture = 0;
        return Result<bool>::Success(false);
    }

    const Vec2 viewport(static_cast<float>(host.window->Width()),
                        static_cast<float>(host.window->Height()));
    auto arranged = host.app.PrepareLayout(Geometry::Root(viewport, {}), 1);
    if (!arranged) {
        return Result<bool>::Failure(arranged.Error());
    }

    auto prepared = host.surfaces.Prepare(host.area, host.frameNumber++, host.window->Width(),
                                          host.window->Height(), pipeline_);
    if (!prepared) {
        return Result<bool>::Failure(prepared.Error());
    }

    repaint |= prepared.Value();
    auto cache = host.app.UpdateArrangedPaintCache();
    if (!cache) {
        return Result<bool>::Failure(cache.Error());
    }

    // 미리보기 표시에 실패해도 원래 탭과 드롭 처리는 유지한다.
    (void)UpdateDragImage(host);
    const bool changed = !host.presented || cache.Value()->Revision() != host.batchRevision;
    if (refreshCursor || changed) {
        host.window->SetMouseCursor(host.adapter.MouseCursor());
    }

    if (changed) {
        auto batched = host.batch.Update(*cache.Value());
        if (!batched) {
            return Result<bool>::Failure(batched.Error());
        }

        host.batchRevision = cache.Value()->Revision();
        repaint = true;
    }

    if (!repaint) {
        return Result<bool>::Success(false);
    }

    auto uploaded = host.buffers.Upload();
    if (!uploaded) {
        return Result<bool>::Failure(uploaded.Error());
    }

    // 그리기 목록이 받은 SVG 자리의 페이지를 올린다(그리기 전 — 17.2절).
    if (auto synced = icons_.Sync(imagePipeline_); !synced) {
        return Result<bool>::Failure(synced.Error());
    }

    // 창 바탕은 테마 Background다(내용이 덮지 않는 틈에 보인다).
    const LinearColor background = host.app.Theme().Get(ThemeSlot::Background);
    auto frame =
        device_.BeginFrame(host.chain, {background.r, background.g, background.b, background.a});
    if (!frame) {
        return Result<bool>::Failure(frame.Error());
    }

    auto drawn =
        DrawBatchRenderer::Draw(*frame.Value(), pipeline_, host.buffers, viewport, &textures_);
    auto presented = device_.EndFrame();
    if (!drawn || !presented) {
        return Result<bool>::Failure(!drawn ? drawn.Error() : presented.Error());
    }

    host.presented = true;
    // 한도를 넘었으면 atlas를 비운다. revision이 올라 다음 그리기가 목록을 다시 만든다.
    (void)icons_.AdvanceFrame();
    return Result<bool>::Success(true);
}

Result<void> DockWindows::ReturnTabs(Surface& surface) noexcept {
    while (surface.area->RegisteredCount()) {
        const auto info = surface.area->TabInfo(0);
        if (!info) {
            return Result<void>::Failure(info.Error());
        }

        auto moved = surface.area->TransferTabTo(info.Value().id, *windows_[primary_]->area);
        if (!moved) {
            return moved;
        }
    }

    return Result<void>::Success();
}

Result<void> DockWindows::Run(UniquePtr<platform::Window> window,
                              const SharedPtr<DockManager>& area) noexcept {
    auto main = Add(Move(window), area);
    if (!main) {
        return Result<void>::Failure(main.Error());
    }

    primary_ = main.Value();
    Surface& primary = *windows_[primary_];
    while (!primary.window->IsCloseRequested()) {
        bool painted = false;
        // 입력 콜백이 새 창을 열 수 있다. 슬롯 배열의 반복자·원소 참조를 콜백 너머로 보관하지
        // 않는다.
        const usize count = windows_.SlotCount();
        for (usize i = 0; i < count; ++i) {
            const auto handle = windows_.HandleAt(i);
            if (handle.IsNull()) {
                continue;
            }

            Surface& surface = *windows_[handle];
            auto updated = Update(surface);
            if (!updated) {
                return Result<void>::Failure(updated.Error());
            }

            painted |= updated.Value();
            if (handle != primary_ &&
                (surface.window->IsCloseRequested() || surface.area->ActiveChildren().IsEmpty())) {
                if (surface.window->IsCloseRequested()) {
                    surface.area->CloseWindowTabs();
                }

                auto moved = ReturnTabs(surface);
                if (!moved) {
                    surface.window->ClearCloseRequest();
                    continue;
                }

                (void)windows_.TryRemove(handle);
            }
        }

        if (!painted && !primary.window->IsCloseRequested()) {
            primary.window->WaitForEvents(50);
        }
    }

    for (const auto& surface : windows_) {
        if (surface.Get() != &primary) {
            auto moved = ReturnTabs(*surface);
            if (!moved) {
                return moved;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
