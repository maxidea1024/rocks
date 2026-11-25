#include "ScreenHost.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/FrameArena.h>
#include <Core/Math/MatrixFactory.h>
#include <Core/Move.h>
#include <D3D11Graphics/D3D11Device.h>
#include <D3D12Graphics/D3D12Device.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstance.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <Gameplay/LocalPlayer.h>
#include <Gameplay/PlayerController.generated.h>
#include <VulkanGraphics/VulkanDevice.h>
#if SANDBOX_OPENGL
#include <OpenGLGraphics/OpenGLDevice.h>
#endif

namespace sandbox::screen {

namespace {

inline constexpr LogCategory LogSandboxScreen{"Sandbox.Screen"};

// Sandbox 장면이다(18.5절 1항). 카메라(PlayerStart 원점, +X를 본다) 앞 500 cm에 cube 셋, 그 위에
// Pending 텍스처 cube 하나, 아래 100 cm에 바닥 plane(20 m × 20 m, 중간 회색 0.18)을 둔다.
constexpr SceneObject Scene[] = {
    {render::BuiltinMesh::Plane,
     Vec3d(SceneCubeDistance, 0.0, -100.0),
     Vec3d(20.0, 20.0, 1.0),
     {0.18f, 0.18f, 0.18f, 1.0f}},
    {render::BuiltinMesh::Cube,
     Vec3d(SceneCubeDistance, 0.0, 0.0),
     Vec3d(1.0, 1.0, 1.0),
     {SceneCenterColor[0], SceneCenterColor[1], SceneCenterColor[2], 1.0f}},
    {render::BuiltinMesh::Cube,
     Vec3d(SceneCubeDistance, -SceneRightOffset, 0.0),
     Vec3d(1.0, 1.0, 1.0),
     {0.25f, 1.0f, 4.0f, 1.0f}},
    {render::BuiltinMesh::Cube,
     Vec3d(SceneCubeDistance, SceneRightOffset, 0.0),
     Vec3d(1.0, 1.0, 1.0),
     {SceneRightColor[0], SceneRightColor[1], SceneRightColor[2], 1.0f}},
    {render::BuiltinMesh::Cube,
     Vec3d(SceneCubeDistance, 0.0, 200.0),
     Vec3d(1.0, 1.0, 1.0),
     {1.0f, 1.0f, 1.0f, 1.0f},
     render::BuiltinTexture::White2D,
     true}};

Mat4 ToFloat(const Mat4d& matrix) noexcept {
    Mat4 out = Mat4::Zero();
    for (usize index = 0; index < 16; ++index) {
        out.m[index] = static_cast<float>(matrix.m[index]);
    }

    return out;
}

} // namespace

bool ParseScreenBackend(StringView text, ScreenBackend& backend) noexcept {
    struct Name {
        const char* text;
        ScreenBackend backend;
    };

    static constexpr Name Names[] = {{"auto", ScreenBackend::Auto},
                                     {"d3d12", ScreenBackend::D3D12},
                                     {"d3d11", ScreenBackend::D3D11},
                                     {"vulkan", ScreenBackend::Vulkan},
                                     {"opengl", ScreenBackend::OpenGL}};
    for (const Name& name : Names) {
        if (text == StringView(name.text)) {
            backend = name.backend;
            return true;
        }
    }

    return false;
}

const char* ScreenBackendName(ScreenBackend backend) noexcept {
    switch (backend) {
    case ScreenBackend::Auto:
        return "Auto";
    case ScreenBackend::D3D12:
        return "D3D12";
    case ScreenBackend::D3D11:
        return "D3D11";
    case ScreenBackend::Vulkan:
        return "Vulkan";
    case ScreenBackend::OpenGL:
        return "OpenGL ES";
    }

    return "Unknown";
}

Result<UniquePtr<graphics::Device>> CreateScreenDevice(Allocator& allocator, ScreenBackend backend,
                                                       bool validation) noexcept {
    if (backend == ScreenBackend::Auto) {
        constexpr ScreenBackend Order[] = {ScreenBackend::D3D12, ScreenBackend::Vulkan,
                                           ScreenBackend::OpenGL};
        Result<UniquePtr<graphics::Device>> created =
            Result<UniquePtr<graphics::Device>>::Failure(ErrorCode::Unsupported);
        for (ScreenBackend candidate : Order) {
            created = CreateScreenDevice(allocator, candidate, validation);
            if (created) {
                break;
            }
        }

        return created;
    }

    graphics::DeviceDesc desc;
    desc.enableValidation = validation;
    graphics::DeviceCreateReport report;
    Result<UniquePtr<graphics::Device>> created =
        Result<UniquePtr<graphics::Device>>::Failure(ErrorCode::Unsupported);
    switch (backend) {
    case ScreenBackend::D3D12:
        created = graphics::CreateD3D12Device(allocator, desc, &report);
        break;
    case ScreenBackend::D3D11:
        created = graphics::CreateD3D11Device(allocator, desc, &report);
        break;
    case ScreenBackend::Vulkan:
        created = graphics::CreateVulkanDevice(allocator, desc, &report);
        break;
    case ScreenBackend::Auto:
        break;
    case ScreenBackend::OpenGL:
#if SANDBOX_OPENGL
        created = graphics::CreateOpenGLDevice(allocator, desc, &report);
#endif
        break;
    }

    if (!created) {
        Log::Warning(LogSandboxScreen,
                     "The {Backend} device could not be created at {Stage}: {Error} ({Detail})",
                     ScreenBackendName(backend), StringView(report.stage),
                     ErrorCodeName(created.Error()), StringView(report.detail));
    }

    return created;
}

Result<UniquePtr<graphics::Device>> CreateScreenDeviceInOrder(Allocator& allocator,
                                                              Span<const graphics::Backend> order,
                                                              bool validation) noexcept {
    Result<UniquePtr<graphics::Device>> created =
        Result<UniquePtr<graphics::Device>>::Failure(ErrorCode::Unsupported);
    for (const graphics::Backend backend : order) {
        ScreenBackend screen = ScreenBackend::Auto;
        switch (backend) {
        case graphics::Backend::D3D12:
            screen = ScreenBackend::D3D12;
            break;
        case graphics::Backend::D3D11:
            screen = ScreenBackend::D3D11;
            break;
        case graphics::Backend::Vulkan:
            screen = ScreenBackend::Vulkan;
            break;
        case graphics::Backend::OpenGL:
            screen = ScreenBackend::OpenGL;
            break;
        case graphics::Backend::Metal:
            break;
        }

        if (screen == ScreenBackend::Auto) {
            continue;
        }

        created = CreateScreenDevice(allocator, screen, validation);
        if (created) {
            break;
        }
    }

    return created;
}

bool CanOpenEngineShaders(const render::EngineShaderHost& host) noexcept {
    return host.Provider() != nullptr || host.CookedLibraries() != nullptr;
}

Result<ScreenView> MakeScreenView(const CameraView& camera, uint32 width, uint32 height) noexcept {
    if (width == 0 || height == 0) {
        return Result<ScreenView>::Failure(ErrorCode::InvalidArgument);
    }

    // UE 기본 종횡비 축(수평 FOV 유지): 화면 비율로 세로 계수를 정한다(18.5절 2항).
    auto projection =
        TryMakePerspective(math::DegreesToRadians(camera.fov) * 0.5, static_cast<double>(width),
                           static_cast<double>(height), ScreenNearPlane);
    if (!projection) {
        return Result<ScreenView>::Failure(projection.Error());
    }

    const Mat4d viewRotation = MakeViewRotation(camera.rotation);
    ScreenView view;
    view.origin = camera.location;
    view.relativeWorldToClip = viewRotation * projection.Value();
    view.mirrored = viewRotation.Determinant() < 0.0;
    return Result<ScreenView>::Success(view);
}

bool FindPlayerCamera(GameInstanceClient& client, CameraView& camera) noexcept {
    auto found = client.WithInstance([&camera](GameInstance& instance) noexcept -> bool {
        World* world = instance.CurrentWorld();
        if (world == nullptr || instance.LocalPlayerCount() == 0) {
            return false;
        }

        ObjectHandle controller;
        (void)instance.WithLocalPlayer(instance.LocalPlayerAt(0),
                                       [&controller](LocalPlayer& player) noexcept {
                                           controller = player.PlayerControllerHandle();
                                       });
        auto director =
            world->With<PlayerController>(controller, [](PlayerController& player) noexcept {
                return player.CameraDirectorHandle();
            });
        if (!director) {
            return false;
        }

        auto view = world->With<CameraDirector>(
            director.Value(), [](CameraDirector& value) noexcept { return value.View(); });
        if (!view) {
            return false;
        }

        camera = view.Value();
        return true;
    });
    return found && found.Value();
}

Span<const SceneObject> SandboxScene() noexcept {
    return Span<const SceneObject>(Scene);
}

ScreenHost::ScreenHost(Allocator& allocator, graphics::Device& device,
                       const render::EngineShaderHost& host) noexcept
    : device_(device), renderer_(allocator, host), lane_(allocator, device) {
}

ScreenHost::~ScreenHost() noexcept {
    if (!started_) {
        return;
    }

    // 엔진 종료 hook을 거치지 않은 경우(엔진 초기화 실패 등)의 같은 종료 순서다(5.5절).
    BlockFrames();
    (void)WaitFramesIdle();
    for (;;) {
        auto drained = DrainRetires();
        if (!drained || drained.Value() == 0) {
            break;
        }
    }

    (void)CloseResources();
    (void)Stop();
}

Result<void> ScreenHost::Start(platform::Window& window, render::RenderLaneMode mode,
                               render::RenderLanePump pump) noexcept {
    const platform::NativeWindowHandle native = window.NativeHandle();
    graphics::NativeSurface surface;
    surface.window = native.window;
    surface.instance = native.instance;
    width_ = window.Width();
    height_ = window.Height();
    renderer_.SetInitialSurface(surface, width_, height_);
    render::RenderLaneConfig config;
    config.mode = mode;
    config.processor = &renderer_;
    config.pump = pump;
    if (auto started = lane_.Start(config); !started) {
        Log::Error(LogSandboxScreen, "The render lane could not start: {Error}",
                   ErrorCodeName(started.Error()));
        return started;
    }

    window_ = &window;
    started_ = true;
    // 다음 단계의 쿠킹 텍스처가 들어올 자리의 Pending handle이다(장치 자원을 잇지 않는다 — typed
    // 대체로 그려진다).
    auto pending = lane_.ReserveHandle(render::RenderResourceKind::Texture);
    if (pending) {
        pending_ = pending.Value();
    }

    Log::Info(LogSandboxScreen, "Screen rendering started on {Backend} ({Width}x{Height}, {Mode})",
              graphics::BackendName(device_.Info().backend), width_, height_,
              mode == render::RenderLaneMode::Thread ? "thread" : "inline");
    return Result<void>::Success();
}

void ScreenHost::SubmitFrame(GameInstanceClient* client) noexcept {
    if (!started_ || failed_ || surfaceLost_ || width_ == 0 || height_ == 0) {
        return;
    }

    auto ticket = lane_.BeginFrame();
    if (!ticket) {
        failed_ = true;
        Log::Error(LogSandboxScreen, "Screen rendering stopped: the render lane failed ({Error})",
                   ErrorCodeName(ticket.Error()));
        return;
    }

    CameraView camera;
    if (client != nullptr) {
        (void)FindPlayerCamera(*client, camera);
    }

    auto view = MakeScreenView(camera, width_, height_);
    const Span<const SceneObject> scene = SandboxScene();
    FrameArena& arena = lane_.Arena(ticket.Value());
    auto items = arena.TryNewArray<ScreenItem>(scene.Size());
    if (!view || !items) {
        lane_.AbandonFrame(Move(ticket.Value()));
        return;
    }

    for (usize index = 0; index < scene.Size(); ++index) {
        const SceneObject& object = scene[index];
        ScreenItem& item = items.Value()[index];
        // 카메라 상대 local → world(회전 없음): scale 뒤 (위치 − 카메라 위치)로 옮긴다.
        Mat4d local = Mat4d::Scale(object.scale);
        const Vec3d relative = object.location - view.Value().origin;
        local.m[12] = relative.x;
        local.m[13] = relative.y;
        local.m[14] = relative.z;
        const Mat4 localToClip = ToFloat(local * view.Value().relativeWorldToClip);
        for (uint32 row = 0; row < 4; ++row) {
            for (uint32 column = 0; column < 4; ++column) {
                item.constants.localToClip[row][column] = localToClip.m[row * 4 + column];
            }
        }

        for (uint32 channel = 0; channel < 4; ++channel) {
            item.constants.color[channel] = object.color[channel];
        }

        item.mesh = object.mesh;
        item.builtin = object.builtin;
        item.texture = object.pendingTexture ? pending_ : render::RenderHandle{};
        // 물체 행렬식과 view 행렬식 부호가 다르면 감김이 뒤집힌다(UE StaticMeshSceneProxy).
        item.reverseCull = (local.Determinant() < 0.0) != view.Value().mirrored;
    }

    ScreenPacket packet;
    packet.items = items.Value();
    packet.clear = {SceneClearColor[0], SceneClearColor[1], SceneClearColor[2], 1.0f};
    packet.tonemap = tonemap_;
    packet.capture = capture_;
    packet.captureSlot = captureSlot_;
    packet.inspector = inspector_;
    packet.inspectorLayer = inspectorLayer_;
    packet.inspectorMip = inspectorMip_;
    if (!lane_.SubmitFrame(Move(ticket.Value()), Move(packet), &RenderScreen)) {
        failed_ = true;
        Log::Error(LogSandboxScreen, "A screen frame could not be submitted");
        return;
    }

    ++submitted_;
}

void ScreenHost::SetInspector(InspectorView view, uint32 layer, uint32 mip) noexcept {
    const uint32 index = static_cast<uint32>(view);
    if (index >= InspectorViewCount) {
        return;
    }

    inspector_ = view;
    const uint32 layers = InspectorLayers[index];
    const uint32 mips = InspectorMips[index];
    inspectorLayer_ = layers == 0 ? 0 : (layer < layers ? layer : layers - 1);
    inspectorMip_ = mips == 0 ? 0 : (mip < mips ? mip : mips - 1);
}

void ScreenHost::HandleInspectorInput(const input::InputState& input) noexcept {
    constexpr input::Key ViewKeys[InspectorViewCount] = {
        input::Key::Digit0, input::Key::Digit1, input::Key::Digit2, input::Key::Digit3,
        input::Key::Digit4, input::Key::Digit5, input::Key::Digit6};
    constexpr uint32 FirstGamepad = 0;
    InspectorView view = inspector_;
    uint32 layer = inspectorLayer_;
    uint32 mip = inspectorMip_;
    for (uint32 index = 0; index < InspectorViewCount; ++index) {
        if (input.WasPressed(ViewKeys[index])) {
            view = static_cast<InspectorView>(index);
            layer = 0;
            mip = 0;
        }
    }

    if ((input.WasPressed(input::Key::LeftBracket) ||
         input.WasPressed(FirstGamepad, input::GamepadButton::LeftShoulder)) &&
        layer != 0) {
        --layer;
    }

    if (input.WasPressed(input::Key::RightBracket) ||
        input.WasPressed(FirstGamepad, input::GamepadButton::RightShoulder)) {
        ++layer;
    }

    if (input.WasPressed(input::Key::Minus) && mip != 0) {
        --mip;
    }

    if (input.WasPressed(input::Key::Equals)) {
        ++mip;
    }

    if (view != inspector_ || layer != inspectorLayer_ || mip != inspectorMip_) {
        SetInspector(view, layer, mip);
        Log::Info(LogSandboxScreen, "Texture inspector view {View} layer {Layer} mip {Mip}",
                  static_cast<uint32>(inspector_), inspectorLayer_, inspectorMip_);
    }
}

void ScreenHost::Resize(uint32 width, uint32 height) noexcept {
    if (!started_ || width == 0 || height == 0) {
        return;
    }

    width_ = width;
    height_ = height;
    (void)lane_.PostSurface(SurfaceResize{width, height}, &ResizeSurface);
}

void ScreenHost::ReleaseSurface() noexcept {
    if (!started_ || surfaceLost_) {
        return;
    }

    surfaceLost_ = true;
    if (auto released = lane_.PostSurfaceAndWait(SurfaceRelease{}, &screen::ReleaseSurface);
        !released) {
        Log::Error(LogSandboxScreen, "The swap chain could not be released: {Error}",
                   ErrorCodeName(released.Error()));
    }
}

void ScreenHost::RecreateSurface(platform::Window& window) noexcept {
    if (!started_ || !surfaceLost_) {
        return;
    }

    const platform::NativeWindowHandle native = window.NativeHandle();
    SurfaceCreate create;
    create.surface.window = native.window;
    create.surface.instance = native.instance;
    create.width = window.Width();
    create.height = window.Height();
    if (create.width == 0 || create.height == 0) {
        return;
    }

    width_ = create.width;
    height_ = create.height;
    surfaceLost_ = false;
    (void)lane_.PostSurface(Move(create), &CreateSurface);
}

Result<void> ScreenHost::RequestCapture(uint32 width, uint32 height) noexcept {
    if (!started_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto handle = lane_.ReserveHandle(render::RenderResourceKind::Texture);
    if (!handle) {
        return Result<void>::Failure(handle.Error());
    }

    capture_ = handle.Value();
    return lane_.PostResource(CaptureCreate{capture_, width, height}, &CreateCapture);
}

void ScreenHost::BlockFrames() noexcept {
    if (started_) {
        lane_.BlockFrames();
    }
}

Result<void> ScreenHost::WaitFramesIdle() noexcept {
    return started_ ? lane_.WaitFramesIdle() : Result<void>::Success();
}

Result<uint64> ScreenHost::DrainRetires() noexcept {
    if (!started_) {
        return Result<uint64>::Success(0);
    }

    // 실행 파일이 예약한 handle을 놓는다(엔진 최종 GC가 에셋을 파괴해 생기는 Retire와 같은 자리).
    if (!pendingRetired_) {
        pendingRetired_ = true;
        if (!pending_.IsNull()) {
            (void)lane_.Retire(pending_);
        }

        if (!capture_.IsNull()) {
            (void)lane_.Retire(capture_);
        }
    }

    return lane_.DrainRetires();
}

Result<void> ScreenHost::CloseResources() noexcept {
    return started_ ? lane_.CloseResources() : Result<void>::Success();
}

Result<void> ScreenHost::Stop() noexcept {
    if (!started_) {
        return Result<void>::Success();
    }

    auto stopped = lane_.Stop();
    started_ = false;
    return stopped;
}

} // namespace sandbox::screen
