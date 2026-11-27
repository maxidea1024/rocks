#include "DemoLane.h"
#include "DemoLog.h"
#include "DemoFiles.h"
#include "DemoShaders.h"
#include "DockWindows.h"
#include "GameHud.h"
#include "Gallery/GalleryCapture.h"
#include "Gallery/GallerySelfTest.h"
#include "Gallery/GalleryTextures.h"
#include "Gallery/GalleryWindow.h"
#include "SurfaceSelfTest.h"

#include <AssetDatabase/AtomicFileWrite.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/NumberText.h>
#include <Core/FileRead.h>
#include <Core/ScopeExit.h>
#include <D3D11Graphics/D3D11Device.h>
#include <Launch/EntryPoint.h>
#include <UiRender/LaneUiRenderer.h>
#include <UiRender/VectorImageAtlas.h>

namespace {

using namespace rocks;
using namespace rocks::ui;

// UI 갤러리 창 크기다(Docs/Design/UiToolkit 16장).
// 갤러리 창 제목이다(직접 그린 제목 막대와 OS 작업 표시줄에 같이 쓴다).
constexpr StringView GalleryTitle = "Rocks UI Gallery";
constexpr uint32 GalleryWidth = 1280;
constexpr uint32 GalleryHeight = 800;
// 탭 배치 저장 파일과 형식 판이다(갤러리 탭 ID가 바뀌면 판을 올린다).
constexpr uint32 LayoutVersion = 1;
constexpr StringView LayoutDirectory = "Saved/UiDemo";
constexpr StringView LayoutFile = "Gallery.v1.json";
constexpr StringView LayoutPath = "Saved/UiDemo/Gallery.v1.json";
constexpr usize LayoutFileLimit = 1024 * 1024;
constexpr StringView TabArgument = "--tab=";
// 화면 캡처 높이다(긴 탭의 아래쪽까지 보려고 — 기본 GalleryHeight).
constexpr StringView SnapshotHeightArgument = "--snapshot-height=";
// 렌더 lane 작업을 게임 스레드에서 돌린다(RenderLaneMode::Inline — Thread 모드 결과와 비교용).
constexpr StringView InlineLaneArgument = "--lane=inline";
// 창마다 단계별 프레임 시간을 로그로 남긴다(Docs/Design/UiToolkit 19.5절 — 진단용).
constexpr StringView FrameStatsArgument = "--frame-stats";
constexpr uint32 MaxSnapshotHeight = 8192;

int Run(launch::ApplicationContext& context) noexcept {
    auto& allocator = context.DefaultAllocator();
    bool snapshot = false;
    uint32 snapshotHeight = GalleryHeight;
    bool selfTest = false;
    bool surfaceTest = false;
    bool inlineLane = false;
    bool frameStats = false;
    StringView startTab;
    for (StringView argument : context.Arguments()) {
        if (argument == "--surface-test") {
            surfaceTest = true;
        }

        if (argument == "--self-test") {
            selfTest = true;
        }

        // --tab=<탭 ID>는 창을 그 탭으로 연다(사람 확인·화면 캡처용).
        if (argument.StartsWith(TabArgument)) {
            startTab = argument.SliceBytes(TabArgument.ByteCount(),
                                           argument.ByteCount() - TabArgument.ByteCount());
        }

        if (argument == "--snapshot") {
            snapshot = true;
        }

        if (argument == InlineLaneArgument) {
            inlineLane = true;
        }

        if (argument == FrameStatsArgument) {
            frameStats = true;
        }

        if (argument.StartsWith(SnapshotHeightArgument)) {
            auto height = TryParseUInt32(
                argument.SliceBytes(SnapshotHeightArgument.ByteCount(),
                                    argument.ByteCount() - SnapshotHeightArgument.ByteCount()));
            if (!height || height.Value() == 0 || height.Value() > MaxSnapshotHeight) {
                Log::Error(LogUiDemo, "Invalid snapshot height: {Argument}", argument);
                return 2;
            }

            snapshotHeight = height.Value();
        }
    }

    // 창을 띄우는 실행은 모니터별 DPI를 인식한다(Docs/Design/UiToolkit 19.2절). 결정적이어야 하는
    // 시험·캡처는 배율 1로 둔다.
    if (!snapshot && !selfTest && !surfaceTest) {
        (void)platform::EnableHighDpiAwareness();
    }

    // 엔진 셰이더 호스트(셰이더 타입 등록·Development 공급자 — RenderBase 9.12절 RR11)다.
    DemoShaderHost shaderHost(context.Modules());
    if (!shaderHost.IsReady()) {
        return 2;
    }

    auto created = graphics::CreateD3D11Device(
        allocator, {.preferSoftware = snapshot || selfTest || surfaceTest});
    if (!created) {
        return 1;
    }

    // 장치는 렌더 lane이 소유하고 UI는 LaneUiRenderer로만 그린다(Docs/Design/UiToolkit 18.5절).
    // --lane=inline은 lane 작업을 이 스레드에서 돌린다(Thread 모드와 같은 결과인지 비교할 때).
    DemoLaneProcessor processor(allocator);
    render::RenderLane lane(allocator, *created.Value());
    render::RenderLaneConfig laneConfig;
    laneConfig.mode = inlineLane ? render::RenderLaneMode::Inline : render::RenderLaneMode::Thread;
    laneConfig.processor = &processor;
    laneConfig.pump = DemoLaneProcessor::Pump();
    if (auto started = lane.Start(laneConfig); !started) {
        Log::Error(LogUiDemo, "The render lane could not start: {Error}",
                   ErrorCodeName(started.Error()));
        return 2;
    }

    // 렌더러 자원을 쓰는 객체(아래에서 만든다)가 모두 부서진 뒤, 장치보다 먼저 lane을 닫는다.
    ROCKS_ON_SCOPE_EXIT {
        if (auto shut = lane.Shutdown(); !shut) {
            Log::Error(LogUiDemo, "The render lane did not shut down cleanly: {Error}",
                       ErrorCodeName(shut.Error()));
        }
    };
    LaneUiRenderer renderer(allocator, lane, processor.Ui());
    if (selfTest) {
        if (const int checked = RunGameHudSelfTest(allocator, renderer); checked != 0) {
            return checked;
        }
    }

    if (surfaceTest || selfTest) {
        // 다시 만들기에 실패해도 옛 파이프라인으로 그리는지 아래 표면 검사가 이어서 본다.
        if (const int reloadChecked = VerifyShaderReload(renderer, lane); reloadChecked != 0) {
            return reloadChecked;
        }

        if (const int presentChecked = VerifyWindowPresents(allocator, renderer, lane);
            presentChecked != 0) {
            return presentChecked;
        }

        const int checked = VerifyWidgetSurfaces(allocator, renderer, lane);
        if (checked != 0 || surfaceTest) {
            return checked;
        }
    }

    DemoText font(allocator, renderer);
    if (!font.Initialize()) {
        return 4;
    }

    // 이미지·9분할·장면 견본용 절차 생성 텍스처다(UiToolkit 16.10절).
    gallery::GalleryTextures sampleTextures(renderer);
    if (!sampleTextures.Create(allocator)) {
        return 3;
    }

    // SVG 아이콘 atlas다(Docs/Design/UiToolkit 17장). 엔진 아이콘 폴더를 모두 등록한다.
    VectorImageAtlas icons(allocator, renderer);
    if (auto registered = RegisterEngineIcons(icons); !registered) {
        Log::Error(LogUiDemo, "Engine icons could not be registered ({Error})", registered.Error());
        return 11;
    }

    gallery::GalleryWindow gallery(allocator, font, sampleTextures, &icons);
    auto area = gallery.Create();
    if (!area) {
        return 6;
    }

    if (selfTest) {
        return gallery::VerifyGallery(allocator, gallery, area.Value());
    }

    if (snapshot) {
        const StringView tabs[] = {
            gallery::GalleryWindow::ColorsTab,   gallery::GalleryWindow::TextTab,
            gallery::GalleryWindow::WidgetsTab,  gallery::GalleryWindow::LayoutTab,
            gallery::GalleryWindow::BehaviorTab, gallery::GalleryWindow::EffectsTab,
            gallery::GalleryWindow::IconsTab};
        return gallery::CaptureGallery(allocator, renderer, area.Value(), icons, tabs, GalleryWidth,
                                       snapshotHeight)
                   ? 0
                   : 7;
    }

    platform::WindowDesc desc;
    desc.title = GalleryTitle;
    desc.width = GalleryWidth;
    desc.height = GalleryHeight;
    // 에디터처럼 OS 테두리 없이 제목 막대·창 버튼을 직접 그린다(Docs/Design/UiToolkit 9.1절 UW7b).
    desc.frame = platform::WindowFrame::Custom;
    auto window = platform::Window::Create(allocator, desc);
    // 직접 그린 테두리를 지원하지 않는 플랫폼은 OS 테두리로 연다.
    if (!window && window.Error() == ErrorCode::Unsupported) {
        desc.frame = platform::WindowFrame::System;
        window = platform::Window::Create(allocator, desc);
    }

    if (!window) {
        return 8;
    }

    auto& files = platform::PhysicalFileSystem();
    Array<uint8> layoutBytes(allocator);
    bool exists = false;
    if (ReadWholeFile(files, LayoutPath, LayoutFileLimit, layoutBytes, exists) && exists) {
        (void)area.Value()->RestoreLayout(LayoutVersion, layoutBytes.View());
    }

    if (!startTab.IsEmpty()) {
        if (auto activated = area.Value()->ActivateTab(startTab); !activated) {
            Log::Warning(LogUiDemo,
                         "Unknown gallery tab '{Tab}' ({Error}); opening the default tab", startTab,
                         activated.Error());
        }
    }

    {
        DockWindows windows(
            allocator, renderer, gallery.Commands(), icons,
            DockWindows::TitleFactory::CreateLambda(
                [&allocator, &font](StringView title, const FontStyle& style) noexcept
                    -> Result<SharedPtr<TextLabel>> {
                    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
                    if (!label) {
                        return label;
                    }

                    auto shaped = font.SetText(*label.Value(), title, style);
                    return shaped ? Move(label)
                                  : Result<SharedPtr<TextLabel>>::Failure(shaped.Error());
                }));
        windows.SetFrameStats(frameStats);
        auto mainArea = area.Value();
        if (!windows.Run(Move(window.Value()), area.Value(), GalleryTitle,
                         DockWindows::MainMenuFactory::CreateLambda(
                             [&gallery, mainArea](
                                 Application& app,
                                 platform::Window& owner) noexcept -> Result<SharedPtr<Element>> {
                                 auto bar = gallery.BuildMainMenu(app, owner, mainArea);
                                 if (!bar) {
                                     return Result<SharedPtr<Element>>::Failure(bar.Error());
                                 }

                                 return Result<SharedPtr<Element>>::Success(bar.Value());
                             }))) {
            return 9;
        }
    }

    if (!area.Value()->SaveLayout(LayoutVersion, layoutBytes)) {
        return 10;
    }

    return WriteFileAtomically(files, allocator, LayoutDirectory, LayoutFile, layoutBytes.View())
               ? 0
               : 10;
}

const launch::ApplicationDescription& Describe() noexcept {
    static constexpr launch::ApplicationDescription description{
        .name = "UiDemo",
        .kind = launch::ApplicationKind::EngineHost,
        .settings = {.toolLogging = launch::ToolLogging::Stderr},
        .run = &Run};
    return description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
