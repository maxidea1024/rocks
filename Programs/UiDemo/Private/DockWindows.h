#pragma once

#include "DemoCursor.h"
#include "DemoTextInput.h"
#include "FrameStats.h"

#include <PlatformWindow/Window.h>
#include <UiFramework/Docking/DockManager.h>
#include <UiFramework/Application/InputAdapter.h>
#include <UiFramework/Window/WindowChrome.h>
#include <UiCore/DrawBatch.h>
#include <UiCore/StyleRegistry.h>
#include <UiCore/Rendering/SurfaceRecorder.h>
#include <UiRender/VectorImageAtlas.h>

namespace rocks::ui {

// 창마다 입력·배치·창 표면을 소유한다. 글꼴과 브러시 등 렌더러 자원은 창 사이에서 공유한다.
// 그리기는 UiRenderer(RenderLane 위 LaneUiRenderer)로 넘긴다(Docs/Design/UiToolkit 18.5절).
class DockWindows {
public:
    // 글자 레이블을 font로 shaping해 만든다(직접 그린 창 테두리의 제목과 창 버튼 툴팁).
    using TitleFactory = Delegate<Result<SharedPtr<TextLabel>>(StringView, const FontStyle&)>;
    // 주 창 제목 막대 왼쪽에 둘 주 메뉴를 만든다(그 창의 Application과 창 — UiToolkit 9.1절 UW7c).
    using MainMenuFactory = Delegate<Result<SharedPtr<Element>>(Application&, platform::Window&)>;

    // icons는 SVG brush를 래스터화할 atlas다(Docs/Design/UiToolkit 17.2절). renderer와 icons는
    // 창들보다 오래 살아야 한다.
    DockWindows(Allocator& allocator, UiRenderer& renderer, const SharedPtr<CommandList>& commands,
                VectorImageAtlas& icons, TitleFactory&& makeTitle) noexcept;
    // title은 직접 그린 창 테두리(WindowFrame::Custom)의 제목이다.
    // makeMenu는 주 창의 주 메뉴다(비우면 메뉴 없이).
    Result<void> Run(UniquePtr<platform::Window> window, const SharedPtr<DockManager>& area,
                     StringView title, MainMenuFactory&& makeMenu = {}) noexcept;
    // 창마다 단계별 프레임 시간을 로그로 남긴다(--frame-stats, Docs/Design/UiToolkit 19.5절). Run
    // 전에 부른다.
    void SetFrameStats(bool enabled) noexcept;

private:
    struct Surface {
        Surface(Allocator& allocator, UiRenderer& renderer, UniquePtr<platform::Window> window,
                const SharedPtr<DockManager>& area) noexcept;
        ~Surface() noexcept;
        UiRenderer& renderer;
        UniquePtr<platform::Window> window;
        SharedPtr<DockManager> area;
        // 직접 그린 창 테두리다(창이 WindowFrame::Custom일 때만 — 루트가 된다).
        SharedPtr<WindowChrome> chrome;
        Application app;
        InputAdapter adapter;
        input::InputQueue queue;
        Array<uint8> consumed;
        DrawBatch batch;
        SurfaceRecorder surfaces;
        uint64 frameNumber = 0;
        UiWindowSurfaceId surface{};
        UniquePtr<platform::DragImage> dragImage;
        uint64 previewGesture = 0;
        IntVec2 previewPosition{};
        uint64 batchRevision = 0;
        bool presented = false;
        // 창에 마지막으로 게시한 영역 지도다(바뀐 때만 다시 게시한다).
        input::WindowRegionMap regions;
        // 크기 조절 modal 루프 안의 실시간 그리기가 실패한 원인이다(Run이 다음 차례에 돌려준다).
        ErrorCode liveResizeError = ErrorCode::None;
        // 창의 텍스트 입력 서비스다(19.2절 — 만들지 못하면 IME 글자가 Text 이벤트로 온다).
        SharedPtr<DemoTextInput> textInput;
        // 창의 커서 서비스다(23.5절 — 끄는 동안 커서 숨김·되돌림).
        SharedPtr<DemoCursor> cursor;
        // --frame-stats 단계별 시간이다(제출은 모든 창을 한 번에 하므로 DockWindows가 센다).
        FrameStats stats;
        // 로그에서 창을 가르는 번호다(만든 순서 — 0은 주 창).
        uint32 statsIndex = 0;
    };
    Result<SlotHandle> Add(UniquePtr<platform::Window> window, const SharedPtr<DockManager>& area,
                           StringView title) noexcept;
    // Custom 창이면 제목 막대와 테두리를 만들어 루트로 둔다(창 버튼은 그 창의 동작에 건다).
    // OS 테두리 주 창의 내용 위 줄에 주 메뉴를 둔다.
    Result<void> AttachMenuRow(Surface& host) noexcept;
    Result<void> AttachChrome(Surface& surface, StringView title, bool main) noexcept;
    Result<bool> Update(Surface& surface, UiDrawBuffer& buffer) noexcept;
    // 펌프 없이 크기 반영·배치·표면 기록을 하고, 다시 그려야 하면 창 대상을 buffer에 쌓는다(true).
    // 크기 조절 콜백(UiToolkit 9.1절 UW7a)도 쓴다.
    Result<bool> Render(Surface& surface, bool refreshCursor, UiDrawBuffer& buffer) noexcept;
    // buffer에 쌓인 대상을 한 번에 제출하고 비운다(atlas 업로드는 그 앞, 프레임 진행은 그 뒤).
    Result<void> DrawFrame(UiDrawBuffer& buffer) noexcept;
    Result<void> UpdateDragImage(Surface& surface) noexcept;
    Result<void> ReturnTabs(Surface& surface) noexcept;
    // 모든 창의 다음 타이머 마감까지 기다릴 시간(ms)이다(상한 있음).
    uint32 IdleWaitMilliseconds() const noexcept;
    SharedPtr<DockManager> FindArea(Surface& source, Vec2 point, Vec2& clientPoint) noexcept;
    Result<void> FloatTab(Surface& source, StringView id, Vec2 point) noexcept;
    Allocator& allocator_;
    UiRenderer& renderer_;
    SharedPtr<CommandList> commands_;
    VectorImageAtlas& icons_;
    TitleFactory makeTitle_;
    MainMenuFactory makeMenu_;
    SlotArray<UniquePtr<Surface>> windows_;
    // --frame-stats이면 창마다·제출의 단계별 시간을 남긴다.
    bool frameStats_ = false;
    FrameStats submitStats_;
    // 다음 창의 frame-stats 번호다.
    uint32 nextStatsIndex_ = 0;
    // 모든 창이 함께 쓰는 스타일 등록부다(첫 창을 만들 때 — 앱 스타일은 EditorStyle, 20.11절).
    SharedPtr<StyleRegistry> styles_;
    SlotHandle primary_{};
};

} // namespace rocks::ui
