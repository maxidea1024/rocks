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
#include <Core/FileRead.h>
#include <Core/ScopeExit.h>
#include <D3D11Graphics/D3D11Device.h>
#include <Launch/EntryPoint.h>
#include <UiRender/VectorImageAtlas.h>

namespace {

using namespace rocks;
using namespace rocks::ui;

// UI 갤러리 창 크기다(Docs/Design/UiToolkit 16장).
constexpr uint32 GalleryWidth = 1280;
constexpr uint32 GalleryHeight = 800;
// 탭 배치 저장 파일과 형식 판이다(갤러리 탭 ID가 바뀌면 판을 올린다).
constexpr uint32 LayoutVersion = 1;
constexpr StringView LayoutDirectory = "Saved/UiDemo";
constexpr StringView LayoutFile = "Gallery.v1.json";
constexpr StringView LayoutPath = "Saved/UiDemo/Gallery.v1.json";
constexpr usize LayoutFileLimit = 1024 * 1024;
constexpr StringView TabArgument = "--tab=";

int Run(launch::ApplicationContext& context) noexcept {
    auto& allocator = context.DefaultAllocator();
    bool snapshot = false;
    bool selfTest = false;
    bool surfaceTest = false;
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
    }

    // 엔진 셰이더 호스트(셰이더 타입 등록·Development 공급자 — RenderBase 9.12절 RR11)다.
    DemoShaderHost shaderHost(context.Modules());
    if (!shaderHost.IsReady()) {
        return 2;
    }

    if (selfTest) {
        const int checked = RunGameHudSelfTest(allocator);
        if (checked != 0) {
            return checked;
        }
    }

    auto created = graphics::CreateD3D11Device(
        allocator, {.preferSoftware = snapshot || selfTest || surfaceTest});
    if (!created) {
        return 1;
    }

    auto& device = *created.Value();
    // 장치의 UI 셰이더 집합이다(UiToolkit 9.1절 UW1a).
    DemoShaderSet shaders(allocator, device);
    if (!shaders.IsReady()) {
        return 2;
    }

    constexpr auto format = graphics::PixelFormat::RGBA8UnormSrgb;
    auto solid = DrawBatchRenderer::CreateSolidPipeline(device, ActiveShaders(), format);
    if (!solid) {
        return 2;
    }

    ROCKS_ON_SCOPE_EXIT {
        device.Retire(solid.Value());
    };
    if (surfaceTest || selfTest) {
        const int checked = VerifyWidgetSurfaces(allocator, device, solid.Value());
        if (checked != 0 || surfaceTest) {
            return checked;
        }
    }

    auto image = DrawBatchRenderer::CreateImagePipeline(device, ActiveShaders(), format);
    if (!image) {
        return 2;
    }

    ROCKS_ON_SCOPE_EXIT {
        device.Retire(image.Value());
    };
    auto mask = DrawBatchRenderer::CreateMaskPipeline(device, ActiveShaders(), format);
    if (!mask) {
        return 2;
    }

    ROCKS_ON_SCOPE_EXIT {
        device.Retire(mask.Value());
    };

    SlotArray<BrushTexture> textures(allocator);
    DemoText font(allocator, device, textures, mask.Value(), image.Value());
    if (!font.Initialize()) {
        return 4;
    }

    // 이미지·9분할·장면 견본용 절차 생성 텍스처다(UiToolkit 16.10절).
    gallery::GalleryTextures sampleTextures(device, textures, image.Value());
    if (!sampleTextures.Create(allocator)) {
        return 3;
    }

    // SVG 아이콘 atlas다(Docs/Design/UiToolkit 17장). 엔진 아이콘 폴더를 모두 등록한다.
    VectorImageAtlas icons(allocator, device, textures);
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
        return gallery::CaptureGallery(allocator, device, solid.Value(), textures, area.Value(),
                                       icons, image.Value(), tabs, GalleryWidth, GalleryHeight)
                   ? 0
                   : 7;
    }

    platform::WindowDesc desc;
    desc.title = "Rocks UI Gallery";
    desc.width = GalleryWidth;
    desc.height = GalleryHeight;
    auto window = platform::Window::Create(allocator, desc);
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
        DockWindows windows(allocator, device, solid.Value(), textures, gallery.Commands(), icons,
                            image.Value());
        if (!windows.Run(Move(window.Value()), area.Value())) {
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
