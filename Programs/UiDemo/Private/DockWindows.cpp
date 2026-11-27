#include "DemoLog.h"
#include "DockWindows.h"

#include <UiFramework/Layout/VerticalStack.h>

#include <Core/Diagnostics/Log.h>
#include <Core/ScopeExit.h>

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

// 끌기 미리보기를 그리는 오프스크린 바탕색이다(이전 직접 읽기 경로와 같은 색).
constexpr LinearColor PreviewClear{0.025f, 0.03f, 0.04f, 1};
// 끌기 미리보기 창의 불투명도(0–255)다.
constexpr uint8 PreviewAlpha = 170;
// 기다릴 마감이 없을 때 쉬는 상한(ms)과 초 → ms 배율이다.
constexpr uint32 MaxIdleWaitMilliseconds = 1000;
constexpr double MillisecondsPerSecond = 1000.0;

} // namespace

DockWindows::Surface::Surface(Allocator& allocator, UiRenderer& uiRenderer,
                              UniquePtr<platform::Window> native,
                              const SharedPtr<DockManager>& manager) noexcept
    : renderer(uiRenderer), window(Move(native)), area(manager), app(allocator), adapter(app),
      queue(allocator), consumed(allocator), batch(allocator), surfaces(allocator, uiRenderer) {
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
    // 편집 칸은 이 창의 IME 문맥이 된다(Docs/Design/UiToolkit 19.2절).
    auto service = SharedPtr<DemoTextInput>::TryMake(allocator, *window);
    if (service) {
        textInput = Move(service.Value());
        (void)app.SetTextInputService(textInput);
    }
}

DockWindows::Surface::~Surface() noexcept {
    // 창이 멤버 중 거의 마지막에 파괴되므로 그 전에 이 호스트를 가리키는 콜백을 먼저 끊는다.
    window->SetLiveResizeHandler({});
    // 팝업 공급자는 이 호스트를 잡는다 — area가 더 오래 살 수 있어 먼저 뗀다.
    (void)app.SetPopupWorkArea({});
    // 편집 요소가 서비스를 더 오래 쥘 수 있어 창과의 연결을 먼저 끊는다.
    if (textInput) {
        textInput->Detach();
    }

    area->SetOnExternalDrop({});
    area->SetWindowHitTest({});
    // 창 표면 파괴는 그 창의 프레임이 모두 처리될 때까지 기다린다. 그 뒤 native 창이 부서진다
    // (window 멤버는 이 본문 뒤에 파괴된다 — Docs/Design/UiToolkit 18.4절).
    if (!surface.IsNull()) {
        (void)renderer.DestroyWindowSurface(surface);
    }
}

DockWindows::DockWindows(Allocator& allocator, UiRenderer& renderer,
                         const SharedPtr<CommandList>& commands, VectorImageAtlas& icons,
                         TitleFactory&& makeTitle) noexcept
    : allocator_(allocator), renderer_(renderer), commands_(commands), icons_(icons),
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
             if (window->GetWindowMode() != platform::WindowMode::Windowed) {
                 (void)window->SetWindowMode(platform::WindowMode::Windowed);
             } else {
                 window->ToggleMaximize();
             }
         }),
         .onClose = Delegate<void()>::CreateLambda([window]() noexcept { window->RequestClose(); }),
         // 전체 화면 제목 줄 두 번 누르기는 창 모드로 돌린다(UE RequestToggleFullscreen — 19.2절).
         .onToggleFullscreen = Delegate<void()>::CreateLambda([window]() noexcept {
             (void)window->SetWindowMode(platform::WindowMode::Windowed);
         })});
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
        UniquePtr<Surface>::TryMake(allocator_, allocator_, renderer_, Move(window), area);
    if (!surface) {
        return Result<SlotHandle>::Failure(surface.Error());
    }

    auto& host = *surface.Value();
    host.stats.SetEnabled(frameStats_);
    // 편집기 룩 창이라 UE 에디터처럼 앱 스타일이 EditorStyle이다(Docs/Design/UiToolkit 20.11절).
    if (!styles_) {
        auto styles = CreateStandardStyleRegistry(allocator_);
        if (!styles) {
            return Result<SlotHandle>::Failure(styles.Error());
        }

        styles_ = styles.Value();
    }

    host.app.SetStyleRegistry(styles_);
    if (auto styled = host.app.SetAppStyle(EditorStyleName); !styled) {
        return Result<SlotHandle>::Failure(styled.Error());
    }

    if (auto chromed = AttachChrome(host, title, windows_.IsEmpty()); !chromed) {
        return Result<SlotHandle>::Failure(chromed.Error());
    }

    host.app.SetVectorImageResolver(&icons_);
    host.surfaces.SetVectorImages(&icons_);
    auto initialized = host.app.SetCommands({}, commands_);
    if (initialized) {
        initialized = host.queue.TryInitialize(1024);
    }

    if (!initialized) {
        return Result<SlotHandle>::Failure(initialized.Error());
    }

    // 창 표면 생성은 lane 작업이 끝날 때까지 기다리므로 실패를 바로 안다(탭 띄우기가 패널을
    // 원래 창에 남긴다 — 18.4절).
    auto native = host.window->NativeHandle();
    UiWindowSurfaceDesc desc;
    desc.nativeWindow = native.window;
    desc.nativeInstance = native.instance;
    desc.width = 1u > host.window->Width() ? 1u : host.window->Width();
    desc.height = 1u > host.window->Height() ? 1u : host.window->Height();
    auto created = renderer_.CreateWindowSurface(desc);
    if (!created) {
        return Result<SlotHandle>::Failure(created.Error());
    }

    host.surface = created.Value();
    // 창 테두리를 끄는 동안 호스트 루프가 멈추므로 창이 이 콜백으로 그 창만 다시 그리게 한다.
    host.window->SetLiveResizeHandler(
        platform::Window::LiveResizeHandler::CreateLambda([this, source = &host]() noexcept {
            if (source->liveResizeError != ErrorCode::None) {
                return;
            }

            // 루프의 공용 버퍼는 이 콜백 바깥(PumpMessages를 부른 Run 루프)이 쓰는 중이라 지역
            // 버퍼로 그 창만 그린다. 그 프레임의 present까지 기다린다(OS 크기 변경 루프가 다음
            // 메시지로 가기 전 — Docs/Design/UiToolkit 18.5절).
            UiDrawBuffer buffer(allocator_);
            // 크기 조절 루프 동안에도 프레임 시각이 흐른다(Docs/Design/UiToolkit 20.3절).
            source->app.BeginFrame();
            auto rendered = Render(*source, false, buffer);
            Result<void> drawn = Result<void>::Success();
            if (rendered) {
                drawn = DrawFrame(buffer);
            }

            if (rendered && rendered.Value() && drawn) {
                drawn = renderer_.Flush();
            }

            if (!rendered) {
                source->liveResizeError = rendered.Error();
            } else if (!drawn) {
                source->liveResizeError = drawn.Error();
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
    // 미리보기 크기·위치는 창 좌표(물리 픽셀)이고 테두리·제목 막대는 논리 크기다. WindowDesc 크기는
    // 논리 크기(창이 놓인 모니터의 DPI로 키운다), desktopPosition은 물리 픽셀이다(19.2절).
    const float scale = source.window->DpiScale();
    desc.width = static_cast<uint32>(size.Value().x / scale + inset.x + inset.z);
    desc.height = static_cast<uint32>(size.Value().y / scale + inset.y + inset.w + barHeight);
    desc.positionClientOrigin = true;
    desc.useDesktopPosition = true;
    desc.desktopPosition = {desktop.Value().x - static_cast<int32>(inset.x * scale),
                            desktop.Value().y - static_cast<int32>((inset.y + barHeight) * scale)};
    desc.frame = platform::WindowFrame::Custom;
    auto window = platform::Window::Create(allocator_, desc);
    // 직접 그린 테두리를 지원하지 않는 플랫폼은 OS 테두리로 연다.
    if (!window && window.Error() == ErrorCode::Unsupported) {
        desc.frame = platform::WindowFrame::System;
        desc.width = static_cast<uint32>(size.Value().x / scale);
        desc.height = static_cast<uint32>(size.Value().y / scale);
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

    // 미리보기는 오프스크린 렌더 타깃에 그려 읽는다(창 pass는 스왑 체인이라 다시 읽을 수 없다 —
    // 18.8절). 처음 창 밖으로 나온 한 번만 그리므로 그 자리에서 읽기를 기다린다.
    const uint32 width = static_cast<uint32>(size.Value().x);
    const uint32 height = static_cast<uint32>(size.Value().y);
    auto target = renderer_.CreateRenderTarget(width, height, UiTextureSampling::Image);
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    ROCKS_ON_SCOPE_EXIT {
        renderer_.ReleaseTexture(target.Value());
    };
    // 창 그리기가 쓰는 공용 버퍼와 섞이지 않게 지역 버퍼로 미리보기만 그린다.
    UiDrawBuffer buffer(allocator_);
    result = buffer.AddOffscreen(target.Value(), width, height, PreviewClear, batch);
    // 미리보기가 받은 SVG 자리의 페이지를 그리기 전에 올린다(17.2절 프레임 순서).
    if (result) {
        result = icons_.Upload();
    }

    if (result) {
        result = renderer_.DrawWindows(buffer);
    }

    if (!result) {
        return result;
    }

    auto capture = renderer_.RequestCapture(target.Value());
    if (!capture) {
        return Result<void>::Failure(capture.Error());
    }

    Array<uint8> pixels(allocator_);
    result = renderer_.WaitCapture(capture.Value());
    if (result) {
        result = renderer_.TakeCapture(capture.Value(), pixels);
    }

    if (!result) {
        return result;
    }

    auto image = platform::DragImage::Create(allocator_, *host.window, position.Value(),
                                             size.Value(), pixels.View(), PreviewAlpha);
    if (!image) {
        return Result<void>::Failure(image.Error());
    }

    host.dragImage = Move(image.Value());
    host.previewPosition = position.Value();
    return Result<void>::Success();
}

Result<bool> DockWindows::Update(Surface& host, UiDrawBuffer& buffer) noexcept {
    // OS 메시지는 루프 앞 PumpMessages가 창마다 나눴다. 이 창의 몫만 가져온다(19.2절).
    host.window->TakeEvents(host.queue);
    auto sized = host.consumed.TryResize(host.queue.Events().Size());
    if (!sized) {
        return Result<bool>::Failure(sized.Error());
    }

    const bool hadInput = !host.queue.Events().IsEmpty();
    // 프레임 시각을 입력 처리 전에 정한다(Docs/Design/UiToolkit 20.3절).
    host.app.BeginFrame();
    host.stats.Begin();
    auto routed = host.adapter.RouteBatch(host.queue.Events(), host.consumed.View(),
                                          host.queue.DroppedCount());
    host.queue.Clear();
    // 커서 모양은 입력 처리 직후 정한다(무거운 프레임 처리 동안 OS가 이전 위치의 모양을 쓰지 않게 —
    // 19.5절). 배치로 커서 아래 Element가 바뀌면 Render가 한 번 더 정한다.
    if (hadInput) {
        host.window->SetMouseCursor(host.adapter.MouseCursor());
    }

    host.stats.Mark(FrameStage::Input);
    // 입력 처리 오류는 UI 버그다. 앱을 닫지 않고 남겨 원인을 찾게 한다(넘친 큐는 정상 상황).
    if (!routed && routed.Error() != ErrorCode::CapacityExceeded) {
        Log::Error(LogUiDemo, "Input routing failed: {Error}", ErrorCodeName(routed.Error()));
    }

    if (host.liveResizeError != ErrorCode::None) {
        return Result<bool>::Failure(host.liveResizeError);
    }

    return Render(host, hadInput || !routed, buffer);
}

Result<void> DockWindows::DrawFrame(UiDrawBuffer& buffer) noexcept {
    if (buffer.Targets().IsEmpty()) {
        return Result<void>::Success();
    }

    // 그리기 목록이 받은 SVG 자리의 페이지를 그리기 전에 올린다(17.2절).
    auto drawn = icons_.Upload();
    if (drawn) {
        drawn = renderer_.DrawWindows(buffer);
    }

    buffer.Clear();
    // 한도를 넘었으면 atlas를 비운다. revision이 올라 다음 그리기가 목록을 다시 만든다.
    (void)icons_.AdvanceFrame();
    return drawn;
}

Result<bool> DockWindows::Render(Surface& host, bool refreshCursor, UiDrawBuffer& buffer) noexcept {
    uint32 width = 0, height = 0;
    bool repaint = host.window->ConsumeRepaint();
    if (host.window->ConsumeResize(width, height) && width && height) {
        auto resized = renderer_.ResizeWindowSurface(host.surface, width, height);
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

    // DPI 배율은 창 루트 변환으로 준다(UE 창 루트 변환 AppScale × DPI — Docs/Design/UiToolkit
    // 19.2절). UI 창 좌표 = client 물리 픽셀이므로 입력·영역 지도·작업 영역은 변환하지 않는다. 크기
    // 조절 중 그리기에서도 매번 읽는다(모니터를 옮기는 중 WM_DPICHANGED가 배율을 먼저 바꾼다).
    const float scale = host.window->DpiScale();
    const Vec2 viewport(static_cast<float>(host.window->Width()) / scale,
                        static_cast<float>(host.window->Height()) / scale);
    // 최대화하면 테두리·여백을 빼고 최대화 버튼을 복원 모양으로 바꾼다.
    if (host.chrome) {
        host.chrome->SetMaximized(host.window->IsMaximized());
        host.chrome->SetFullscreen(host.window->GetWindowMode() != platform::WindowMode::Windowed);
        host.chrome->SetActive(host.window->IsActive());
    }

    auto arranged =
        host.app.PrepareLayout(Geometry::Root(viewport, {.m00 = scale, .m11 = scale}), scale);
    if (!arranged) {
        return Result<bool>::Failure(arranged.Error());
    }

    host.stats.Mark(FrameStage::Layout);

    auto prepared = host.surfaces.Record(host.area, host.frameNumber++, host.window->Width(),
                                         host.window->Height(), buffer);
    if (!prepared) {
        buffer.DiscardPending();
        return Result<bool>::Failure(prepared.Error());
    }

    // 쌓인 표면 pass가 있으면(바깥 보관 표면 안쪽만 바뀐 경우 포함) 창과 함께 제출한다. 버리면 표면
    // 기록기는 그렸다고 알지만 타깃은 비어 있게 된다.
    repaint |= prepared.Value() || buffer.PendingPassCount() > 0;
    host.stats.Mark(FrameStage::Record);
    auto cache = host.app.UpdateArrangedPaintCache();
    if (!cache) {
        buffer.DiscardPending();
        return Result<bool>::Failure(cache.Error());
    }

    host.stats.Mark(FrameStage::PaintCache);

    // 이번 배치의 영역 지도를 창에 게시한다(바뀐 때만 — Docs/Design/UiToolkit 19.2절). OS 영역
    // 질의는 UI 트리 대신 이 지도를 읽는다.
    input::WindowRegionMap regions;
    WindowRegionMapStats regionStats;
    host.app.BuildWindowRegionMap(regions, &regionStats);
    if (!regions.SameAs(host.regions)) {
        host.window->PublishRegionMap(regions);
        host.regions = regions;
    }

    host.stats.Mark(FrameStage::Regions);
    host.stats.NoteCandidates(regionStats.candidates);

    // 미리보기 표시에 실패해도 원래 탭과 드롭 처리는 유지한다. 같은 끌기 동안 다시 시도하도록
    // 제스처 기록을 되돌린다.
    if (auto previewed = UpdateDragImage(host); !previewed) {
        host.dragImage.Reset();
        host.previewGesture = 0;
        Log::Warning(LogUiDemo, "The floating tab preview could not be drawn: {Error}",
                     ErrorCodeName(previewed.Error()));
    }

    const bool changed = !host.presented || cache.Value()->Revision() != host.batchRevision;
    if (refreshCursor || changed) {
        host.window->SetMouseCursor(host.adapter.MouseCursor());
    }

    if (changed) {
        auto batched = host.batch.Update(*cache.Value());
        if (!batched) {
            buffer.DiscardPending();
            return Result<bool>::Failure(batched.Error());
        }

        host.batchRevision = cache.Value()->Revision();
        repaint = true;
    }

    host.stats.Mark(FrameStage::Batch);
    host.stats.EndFrame("Window");
    if (!repaint) {
        return Result<bool>::Success(false);
    }

    // 창 바탕은 테마 Background다(내용이 덮지 않는 틈에 보인다). 대상은 버퍼에 쌓고 제출은 루프 끝
    // DrawFrame이 모든 창을 한 번에 한다(Docs/Design/UiToolkit 18.3절).
    const LinearColor background = host.app.Theme().Get(ThemeSlot::Background);
    auto added = buffer.AddWindow(host.surface, host.window->Width(), host.window->Height(),
                                  background, host.batch);
    if (!added) {
        buffer.DiscardPending();
        return Result<bool>::Failure(added.Error());
    }

    host.presented = true;
    return Result<bool>::Success(true);
}

void DockWindows::SetFrameStats(bool enabled) noexcept {
    frameStats_ = enabled;
    submitStats_.SetEnabled(enabled);
}

uint32 DockWindows::IdleWaitMilliseconds() const noexcept {
    double wait = NoTimerDeadline;
    for (const auto& surface : windows_) {
        wait = math::Min(wait, surface->app.SecondsUntilNextTimer());
    }

    // 마감이 없으면 상한까지만 기다린다(창 밖 상태 변화를 늦게나마 본다). 올림해서 마감 전에 깨지
    // 않게 한다.
    const double milliseconds = math::Ceil(wait * MillisecondsPerSecond);
    return milliseconds >= MaxIdleWaitMilliseconds ? MaxIdleWaitMilliseconds
                                                   : static_cast<uint32>(milliseconds);
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
    // 루프 한 번에 다시 그릴 창을 모두 담아 한 번 제출하는 버퍼다(바뀐 창이 없으면 제출하지
    // 않는다).
    UiDrawBuffer frame(allocator_);
    Array<SlotHandle> closed(allocator_);
    while (!primary.window->IsCloseRequested()) {
        bool painted = false;
        closed.Clear();
        // 이 스레드의 OS 메시지를 한 번에 처리해 창마다 나눈다(Docs/Design/UiToolkit 19.2절).
        platform::PumpMessages();
        // 입력 콜백이 새 창을 열 수 있다. 슬롯 배열의 반복자·원소 참조를 콜백 너머로 보관하지
        // 않는다.
        const usize count = windows_.SlotCount();
        for (usize i = 0; i < count; ++i) {
            const auto handle = windows_.HandleAt(i);
            if (handle.IsNull()) {
                continue;
            }

            Surface& surface = *windows_[handle];
            auto updated = Update(surface, frame);
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

                // 버퍼가 이 창의 batch를 가리킬 수 있으므로 제출 뒤에 지운다.
                if (auto marked = closed.TryPushBack(handle); !marked) {
                    return marked;
                }
            }
        }

        submitStats_.Begin();
        auto drawn = DrawFrame(frame);
        submitStats_.Mark(FrameStage::Submit);
        submitStats_.EndFrame("Submit");
        if (!drawn) {
            return drawn;
        }

        for (const SlotHandle handle : closed) {
            (void)windows_.TryRemove(handle);
        }

        if (!painted && !primary.window->IsCloseRequested()) {
            // 다음 타이머·툴팁 마감까지 기다린다(입력·깨움이 먼저 오면 바로 돈다). 다른 창 대기
            // 큐에 남은 이벤트가 있으면 바로 돌아온다.
            platform::WaitForMessages(IdleWaitMilliseconds());
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
