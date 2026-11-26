#include "DemoLog.h"
#include "DemoShaders.h"
#include "DockWindows.h"

#include <UiFramework/Layout/VerticalStack.h>

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
    // 팝업(메뉴·툴팁·콤보)은 그 앵커가 놓인 모니터의 작업 영역 안에만 놓인다(UW7d). UI 좌표는
    // client 픽셀과 같다. 등록에 실패하면 팝업이 창 경계만 지킨다.
    (void)app.SetPopupWorkArea(
        Element::PopupWorkArea::CreateLambda([this](Vec2 point, Box2<float>& area) noexcept {
            IntVec2 client;
            if (!math::TryTruncToInt32(point.x, client.x) ||
                !math::TryTruncToInt32(point.y, client.y)) {
                return false;
            }

            auto work = window->WorkAreaAt(client);
            if (!work) {
                return false;
            }

            const IntRect rect = work.Value();
            area = {{static_cast<float>(rect.min.x), static_cast<float>(rect.min.y)},
                    {static_cast<float>(rect.max.x), static_cast<float>(rect.max.y)}};
            return true;
        }));
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
                         graphics::PipelineHandle imagePipeline, TitleFactory&& makeTitle) noexcept
    : allocator_(allocator), device_(device), pipeline_(pipeline), textures_(textures),
      commands_(commands), icons_(icons), imagePipeline_(imagePipeline),
      makeTitle_(Move(makeTitle)), windows_(allocator) {
}

Result<void> DockWindows::AttachMenuRow(Surface& host) noexcept {
    // OS 테두리 창(직접 그린 테두리를 못 쓰는 플랫폼)의 주 창은 내용 위 줄에 주 메뉴를 둔다(UW7c
    // R11). 루트는 메뉴가 팝업을 열 수 있게 Overlay(팝업 호스트)다.
    auto menu = makeMenu_.Execute(host.app, *host.window);
    if (!menu) {
        return Result<void>::Failure(menu.Error());
    }

    auto root = SharedPtr<Overlay>::TryMake(allocator_, allocator_);
    auto stack = SharedPtr<VerticalStack>::TryMake(allocator_, allocator_);
    if (!root || !stack) {
        return Result<void>::Failure(!root ? root.Error() : stack.Error());
    }

    auto added = stack.Value()->TryAddSlot(menu.Value(), Auto);
    if (added) {
        added = stack.Value()->TryAddSlot(host.area, Fill(1));
    }

    if (added) {
        added = root.Value()->TryAddChild(stack.Value());
    }

    if (!added) {
        return added;
    }

    host.app.SetRoot(root.Value());
    return Result<void>::Success();
}

Result<void> DockWindows::AttachChrome(Surface& host, StringView title, bool main) noexcept {
    if (host.window->Frame() != platform::WindowFrame::Custom) {
        return main && makeMenu_.IsBound() ? AttachMenuRow(host) : Result<void>::Success();
    }

    auto bar = SharedPtr<WindowTitleBar>::TryMake(allocator_, allocator_);
    auto chrome = SharedPtr<WindowChrome>::TryMake(allocator_, allocator_);
    if (!bar || !chrome) {
        return Result<void>::Failure(!bar ? bar.Error() : chrome.Error());
    }

    SharedPtr<TextLabel> label;
    if (makeTitle_.IsBound()) {
        auto made = makeTitle_.Execute(title, DefaultWindowStyle().title.font);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        label = Move(made.Value());
    }

    // 주 창은 제목 막대 왼쪽에 주 메뉴를 둔다(UE 에디터 메인 프레임).
    SharedPtr<Element> menu;
    if (main && makeMenu_.IsBound()) {
        auto made = makeMenu_.Execute(host.app, *host.window);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        menu = Move(made.Value());
    }

    // 창 버튼은 그 창의 동작을 부른다(닫기는 OS 닫기 경로를 탄다). 창이 Surface보다 먼저 사라지지
    // 않으므로 창 포인터를 그대로 잡는다. 전체 화면이면 최대화 자리 버튼이 전체 화면을 끈다.
    platform::Window* window = host.window.Get();
    auto built = bar.Value()->Construct(
        {.title = Move(label),
         .leftContent = Move(menu),
         .canMaximize = host.window->IsResizable(),
         .makeLabel = DockWindows::TitleFactory::CreateLambda(
             [this](StringView text, const FontStyle& font) noexcept {
                 return makeTitle_.Execute(text, font);
             }),
         .onMinimize = Delegate<void()>::CreateLambda([window]() noexcept { window->Minimize(); }),
         .onToggleMaximize = Delegate<void()>::CreateLambda([window]() noexcept {
             if (window->IsFullscreen()) {
                 window->ToggleFullscreen();
             } else {
                 window->ToggleMaximize();
             }
         }),
         .onClose =
             Delegate<void()>::CreateLambda([window]() noexcept { window->RequestClose(); })});
    if (built) {
        built = chrome.Value()->Construct({.titleBar = bar.Value(), .content = host.area});
    }

    if (!built) {
        return built;
    }

    host.chrome = Move(chrome.Value());
    host.app.SetRoot(host.chrome);
    return Result<void>::Success();
}

Result<SlotHandle> DockWindows::Add(UniquePtr<platform::Window> window,
                                    const SharedPtr<DockManager>& area, StringView title) noexcept {
    auto surface =
        UniquePtr<Surface>::TryMake(allocator_, allocator_, device_, Move(window), area, textures_);
    if (!surface) {
        return Result<SlotHandle>::Failure(surface.Error());
    }

    auto& host = *surface.Value();
    if (auto chromed = AttachChrome(host, title, windows_.IsEmpty()); !chromed) {
        return Result<SlotHandle>::Failure(chromed.Error());
    }

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

    // 떠 있는 탭 창도 에디터 창처럼 직접 그린 테두리다(UE 떠 있는 탭 창의 SWindow 제목 막대). 내용
    // (도킹 영역)이 미리보기 자리에 오도록 창은 테두리 여백과 제목 막대만큼 크고 위·왼쪽으로
    // 놓는다(UE SWindow::GetWindowSizeFromClientSize). 제목 높이는 실제 제목 글자로 잰다. 탭 id는
    // 갤러리에서 곧 표시 이름이다(탭에 글자 이름이 생기면 그것을 쓴다 — UW4c).
    const WindowStyle& frameStyle = DefaultWindowStyle();
    float titleHeight = 0;
    if (makeTitle_.IsBound()) {
        if (auto label = makeTitle_.Execute(id, frameStyle.title.font)) {
            titleHeight = label.Value()->GetDesiredSize().y;
            if (titleHeight <= 0) {
                (void)label.Value()->Prepass(1);
                titleHeight = label.Value()->GetDesiredSize().y;
            }
        }
    }

    const Vec4 inset = frameStyle.borderPadding + frameStyle.contentPadding;
    const float barHeight = WindowTitleBar::DesiredHeight(frameStyle, titleHeight);
    platform::WindowDesc desc;
    desc.title = id;
    desc.width = static_cast<uint32>(size.Value().x + inset.x + inset.z);
    desc.height = static_cast<uint32>(size.Value().y + inset.y + inset.w + barHeight);
    desc.positionClientOrigin = true;
    desc.useDesktopPosition = true;
    desc.desktopPosition = {desktop.Value().x - static_cast<int32>(inset.x),
                            desktop.Value().y - static_cast<int32>(inset.y + barHeight)};
    desc.frame = platform::WindowFrame::Custom;
    auto window = platform::Window::Create(allocator_, desc);
    // 직접 그린 테두리를 지원하지 않는 플랫폼은 OS 테두리로 연다.
    if (!window && window.Error() == ErrorCode::Unsupported) {
        desc.frame = platform::WindowFrame::System;
        desc.width = static_cast<uint32>(size.Value().x);
        desc.height = static_cast<uint32>(size.Value().y);
        desc.desktopPosition = desktop.Value();
        window = platform::Window::Create(allocator_, desc);
    }

    if (!window) {
        return Result<void>::Failure(window.Error());
    }

    auto area = SharedPtr<DockManager>::TryMake(allocator_, allocator_);
    if (!area) {
        return Result<void>::Failure(area.Error());
    }

    auto added = Add(Move(window.Value()), area.Value(), id);
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
    // 최대화하면 테두리·여백을 빼고 최대화 버튼을 복원 모양으로 바꾼다.
    if (host.chrome) {
        host.chrome->SetMaximized(host.window->IsMaximized());
        host.chrome->SetFullscreen(host.window->IsFullscreen());
        host.chrome->SetActive(host.window->IsActive());
    }

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
                              const SharedPtr<DockManager>& area, StringView title,
                              MainMenuFactory&& makeMenu) noexcept {
    makeMenu_ = Move(makeMenu);
    auto main = Add(Move(window), area, title);
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
