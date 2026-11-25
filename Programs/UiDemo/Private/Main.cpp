#include "DemoShaders.h"
#include "DockWindows.h"
#include "OrbitWorkspace.h"
#include "WorkspaceCapture.h"
#include "SurfaceSelfTest.h"

#include <AssetDatabase/AtomicFileWrite.h>
#include <Core/FileRead.h>
#include <Core/ScopeExit.h>
#include <D3D11Graphics/D3D11Device.h>
#include <Launch/EntryPoint.h>

namespace {

using namespace rocks;
using namespace rocks::ui;

int Run(launch::ApplicationContext& context) noexcept {
    auto& allocator = context.DefaultAllocator();
    bool snapshot = false;
    bool selfTest = false;
    bool surfaceTest = false;
    for (StringView argument : context.Arguments()) {
        if (argument == "--surface-test") {
            surfaceTest = true;
        }
        if (argument == "--self-test") {
            selfTest = true;
        }
        if (argument == "--snapshot") {
            snapshot = true;
        }
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
    // UI 셰이더 집합(엔진 셰이더 타입 + Development 공급자 — UiToolkit 9.1절 UW1a)이다.
    ROCKS_ON_SCOPE_EXIT {
        ShutdownDemoShaders();
    };
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

    auto map = OrbitView::CreateMap(allocator, device);
    if (!map) {
        return 3;
    }
    ROCKS_ON_SCOPE_EXIT {
        device.Retire(map.Value());
    };
    SlotArray<BrushTexture> textures(allocator);
    auto resource = textures.TryAdd({map.Value(), image.Value()});
    if (!resource) {
        return 3;
    }
    DemoFont font(allocator, device, textures, mask.Value());
    if (!font.Initialize()) {
        return 4;
    }

    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    if (!RegisterWidgetTypes(modules)) {
        return 5;
    }
    ObjectRegistry objects(allocator, types, {0x554944454d4full, 1});
    ObjectAccessScope scope(objects.OwnerAccess());
    OrbitWorkspace workspace(allocator, objects, font);
    auto area = workspace.Create(resource.Value());
    if (!area) {
        return 6;
    }

    platform::WindowDesc desc;
    desc.title = "Rocks UI  |  Orbit workspace";
    desc.width = 1120;
    desc.height = 680;
    if (selfTest) {
        return VerifyWorkspaceInput(allocator, area.Value(), workspace.Commands());
    }
    if (snapshot) {
        return CaptureWorkspace(allocator, device, solid.Value(), textures, area.Value(),
                                desc.width, desc.height)
                   ? 0
                   : 7;
    }
    auto window = platform::Window::Create(allocator, desc);
    if (!window) {
        return 8;
    }

    constexpr uint32 layoutVersion = 2;
    constexpr StringView directory = "Saved/UiDemo";
    constexpr StringView layoutFile = "Workspace.v2.json";
    auto& files = platform::PhysicalFileSystem();
    Array<uint8> layoutBytes(allocator);
    bool exists = false;
    auto read =
        ReadWholeFile(files, "Saved/UiDemo/Workspace.v2.json", 1024 * 1024, layoutBytes, exists);
    if (read && exists) {
        (void)area.Value()->RestoreLayout(layoutVersion, layoutBytes.View());
    } else if (read && !exists) {
        auto legacy = ReadWholeFile(files, "Saved/UiDemo/Workspace.v2.layout", 1024 * 1024,
                                    layoutBytes, exists);
        if (legacy && exists) {
            (void)area.Value()->RestoreLegacyLayout(layoutVersion, layoutBytes.View());
        }
    }
    {
        DockWindows windows(allocator, device, solid.Value(), textures, workspace.Commands());
        if (!windows.Run(Move(window.Value()), area.Value())) {
            return 9;
        }
    }
    if (!area.Value()->SaveLayout(layoutVersion, layoutBytes)) {
        return 10;
    }
    return WriteFileAtomically(files, allocator, directory, layoutFile, layoutBytes.View()) ? 0
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
