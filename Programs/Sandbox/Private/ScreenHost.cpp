#include "ScreenHost.h"

#include "MemoryPlatformTexture.h"

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

// 엔진 절차 텍스처의 색이다(선형 RGBA — RGBA8Unorm에 정확히 담긴다).
constexpr float Red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
constexpr float Green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
constexpr float Blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
constexpr float White[4] = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr float Cyan[4] = {0.0f, 1.0f, 1.0f, 1.0f};
constexpr float Magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
constexpr float Yellow[4] = {1.0f, 1.0f, 0.0f, 1.0f};
// cube 면 순서(+X, -X, +Y, -Y, +Z, -Z)의 색이다.
constexpr const float* CubeFaceColors[6] = {Red, Cyan, Green, Magenta, Blue, Yellow};
constexpr const float* LayerColors[4] = {Red, Green, Blue, White};
// 2D 절차 텍스처의 크기와 mip 수, 배열 층·cube 배열 cube·volume 깊이 수다.
constexpr uint32 Engine2DSize = 4;
constexpr uint32 Engine2DMips = 3;
constexpr uint32 EngineArrayLayers = 3;
constexpr uint32 EngineCubeArrayCubes = 2;
constexpr uint32 EngineVolumeDepth = 4;

void CopyColor(const float* from, float (&value)[4]) noexcept {
    for (uint32 channel = 0; channel < 4; ++channel) {
        value[channel] = from[channel];
    }
}

// 절차 텍스처의 texel이다(context는 InspectorView).
void EngineTexel(void* context, uint32 level, uint32 slice, uint32 x, uint32 y, uint32 z,
                 float (&value)[4]) noexcept {
    switch (*static_cast<const InspectorView*>(context)) {
    case InspectorView::Texture2D:
        CopyColor(level == 0 ? ((x + y) % 2 == 0 ? Red : Green) : (level == 1 ? Blue : White),
                  value);
        return;
    case InspectorView::Hdr2D:
        value[0] = InspectorHdrColor[0];
        value[1] = InspectorHdrColor[1];
        value[2] = InspectorHdrColor[2];
        value[3] = 1.0f;
        return;
    case InspectorView::Texture2DArray:
        CopyColor(LayerColors[slice % 4], value);
        return;
    case InspectorView::Cube:
        CopyColor(CubeFaceColors[slice % graphics::CubeFaceCount], value);
        return;
    case InspectorView::CubeArray:
        CopyColor(slice < graphics::CubeFaceCount ? CubeFaceColors[slice] : White, value);
        return;
    case InspectorView::Volume:
        CopyColor(LayerColors[z % 4], value);
        return;
    case InspectorView::None:
        break;
    }
}

// 절차 텍스처의 메모리 플랫폼 데이터 설정이다(점 표본·clamp — SM6과 같다).
MemoryTextureSpec EngineSpec(InspectorView view) noexcept {
    MemoryTextureSpec spec;
    spec.sampler.filter = TextureSamplerFilter::Point;
    spec.sampler.addressU = TextureAddress::Clamp;
    spec.sampler.addressV = TextureAddress::Clamp;
    spec.sampler.addressW = TextureAddress::Clamp;
    switch (view) {
    case InspectorView::Texture2D:
        spec.width = spec.height = Engine2DSize;
        spec.mips = Engine2DMips;
        break;
    case InspectorView::Hdr2D:
        spec.format = graphics::PixelFormat::RGBA16Float;
        break;
    case InspectorView::Texture2DArray:
        spec.dimension = TextureDimension::Texture2DArray;
        spec.slices = EngineArrayLayers;
        break;
    case InspectorView::Cube:
        spec.dimension = TextureDimension::TextureCube;
        spec.slices = graphics::CubeFaceCount;
        break;
    case InspectorView::CubeArray:
        spec.dimension = TextureDimension::TextureCubeArray;
        spec.slices = graphics::CubeFaceCount * EngineCubeArrayCubes;
        break;
    case InspectorView::Volume:
        spec.dimension = TextureDimension::Volume;
        spec.depth = EngineVolumeDepth;
        break;
    case InspectorView::None:
        break;
    }

    return spec;
}

constexpr const char* EngineTextureNames[InspectorViewCount] = {"",
                                                                "Engine/Texture2D",
                                                                "Engine/Hdr2D",
                                                                "Engine/Texture2DArray",
                                                                "Engine/Cube",
                                                                "Engine/CubeArray",
                                                                "Engine/Volume"};

void CopyName(StringView name, char (&out)[MaxInspectorNameBytes]) noexcept {
    const usize bytes =
        name.ByteCount() < MaxInspectorNameBytes - 1 ? name.ByteCount() : MaxInspectorNameBytes - 1;
    for (usize index = 0; index < bytes; ++index) {
        out[index] = name.Data()[index];
    }

    out[bytes] = '\0';
}

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

bool IsNormalMapSwizzle(const graphics::ChannelSwizzle& swizzle) noexcept {
    return swizzle.red == graphics::SwizzleSource::Red &&
           swizzle.green == graphics::SwizzleSource::Green &&
           swizzle.blue == graphics::SwizzleSource::Zero &&
           swizzle.alpha == graphics::SwizzleSource::One;
}

void DescribeInspectorTexture(graphics::PixelFormat format, const graphics::ChannelSwizzle& swizzle,
                              InspectorTexture& out) noexcept {
    const graphics::PixelFormatInfo& info = graphics::GetPixelFormatInfo(format);
    out.swizzle = swizzle;
    out.integerScale = 1.0f;
    out.display = render::TextureInspectorDisplay::Ldr;
    switch (info.shaderType) {
    case graphics::PixelShaderType::Uint:
    case graphics::PixelShaderType::Int: {
        out.component = info.shaderType == graphics::PixelShaderType::Uint
                            ? InspectorComponent::Uint
                            : InspectorComponent::Int;
        // 성분 bit 수의 최대값(부호 있는 형식은 양수 최대값)으로 나눈다.
        const uint32 texelBytes = info.blockBytes == 0 ? 1u : info.blockBytes;
        uint32 count = 0;
        for (uint8 bits = info.channels; bits != 0; bits = static_cast<uint8>(bits >> 1)) {
            count += bits & 1u;
        }

        const uint32 componentBits = count == 0 ? 8u : (texelBytes * 8u) / count;
        const double maximum = info.shaderType == graphics::PixelShaderType::Uint
                                   ? static_cast<double>((uint64{1} << componentBits) - 1)
                                   : static_cast<double>((uint64{1} << (componentBits - 1)) - 1);
        out.integerScale = static_cast<float>(1.0 / maximum);
        return;
    }

    default:
        out.component = InspectorComponent::Float;
        if (IsNormalMapSwizzle(swizzle)) {
            // Normal sampler type 계약(쿠킹 swizzle rg01)은 형식과 무관하다(BC5·BGRA8·RGBA16F).
            out.display = render::TextureInspectorDisplay::Normal;
        } else if (info.componentType == graphics::PixelComponentType::Float ||
                   info.componentType == graphics::PixelComponentType::Ufloat) {
            out.display = render::TextureInspectorDisplay::Hdr;
        }

        return;
    }
}

ScreenHost::ScreenHost(Allocator& allocator, graphics::Device& device,
                       const render::EngineShaderHost& host) noexcept
    : device_(device), renderer_(allocator, host), lane_(allocator, device),
      ui_(allocator, lane_, renderer_.Ui()), allocator_(allocator), inspectors_(allocator),
      notices_(allocator) {
    for (uint32& entry : engineEntries_) {
        entry = NoInspectorEntry;
    }
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
    // 창 스왑체인은 UI 창 표면이다(18.5절 9항). 생성 대기가 lane 시작 작업(엔진 셰이더 준비)도
    // 기다린다.
    if (auto created = CreateWindowSurface(window); !created) {
        failed_ = true;
        return created;
    }

    if (overlayEnabled_) {
        auto overlay = UniquePtr<ScreenOverlay>::TryMake(allocator_, allocator_);
        if (!overlay) {
            failed_ = true;
            return Result<void>::Failure(overlay.Error());
        }

        if (auto made = overlay.Value()->Create(); !made) {
            failed_ = true;
            Log::Error(LogSandboxScreen, "The screen overlay could not be created: {Error}",
                       ErrorCodeName(made.Error()));
            return made;
        }

        overlay_ = Move(overlay.Value());
    }

    // 다음 단계의 쿠킹 텍스처가 들어올 자리의 Pending handle이다(장치 자원을 잇지 않는다 — typed
    // 대체로 그려진다).
    auto pending = lane_.ReserveHandle(render::RenderResourceKind::Texture);
    if (pending) {
        pending_ = pending.Value();
    }

    AddEngineTextures();

    Log::Info(LogSandboxScreen, "Screen rendering started on {Backend} ({Width}x{Height}, {Mode})",
              graphics::BackendName(device_.Info().backend), width_, height_,
              mode == render::RenderLaneMode::Thread ? "thread" : "inline");
    return Result<void>::Success();
}

void ScreenHost::SubmitFrame(GameInstanceClient* client) noexcept {
    if (!started_ || failed_ || surfaceLost_ || width_ == 0 || height_ == 0) {
        return;
    }

    ProcessNotices();
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
    packet.window = surface_;
    packet.width = width_;
    packet.height = height_;
    if (overlay_ && overlayVisible_) {
        auto recorded = RecordOverlay(arena);
        if (recorded) {
            packet.ui = Move(recorded.Value());
        } else if (!overlayWarned_) {
            overlayWarned_ = true;
            Log::Warning(LogSandboxScreen, "The screen overlay was left out of a frame: {Error}",
                         ErrorCodeName(recorded.Error()));
        }
    }

    packet.items = items.Value();
    packet.clear = {SceneClearColor[0], SceneClearColor[1], SceneClearColor[2], 1.0f};
    packet.tonemap = tonemap_;
    packet.capture = capture_;
    packet.captureSlot = captureSlot_;
    packet.inspecting = selected_ < inspectors_.Size();
    if (packet.inspecting) {
        packet.inspector = inspectors_[selected_].texture;
    }

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
    SetInspectorEntry(index < InspectorViewCount ? engineEntries_[index] : NoInspectorEntry, layer,
                      mip);
}

void ScreenHost::SetInspectorEntry(uint32 index, uint32 layer, uint32 mip) noexcept {
    if (index >= inspectors_.Size()) {
        selected_ = NoInspectorEntry;
        inspectorLayer_ = 0;
        inspectorMip_ = 0;
        return;
    }

    selected_ = index;
    const InspectorTexture& texture = inspectors_[index].texture;
    inspectorMip_ = mip < texture.mips ? mip : texture.mips - 1;
    const uint32 layers = InspectorLayerCount(texture, inspectorMip_);
    inspectorLayer_ = layer < layers ? layer : layers - 1;
}

uint32 ScreenHost::EngineEntry(InspectorView view) const noexcept {
    const uint32 index = static_cast<uint32>(view);
    return index < InspectorViewCount ? engineEntries_[index] : NoInspectorEntry;
}

uint32 ScreenHost::PendingUploads() const noexcept {
    uint32 count = 0;
    for (const InspectorEntry& entry : inspectors_) {
        count += entry.state == InspectorEntryState::Pending ? 1u : 0u;
    }

    return count;
}

void ScreenHost::ProcessNotices() noexcept {
    if (!started_) {
        return;
    }

    notices_.Clear();
    if (!lane_.Notices().TakeAll(notices_)) {
        return;
    }

    if (cache_ != nullptr) {
        (void)cache_->HandleNotices(notices_.View());
    }

    for (const render::RenderNotice& notice : notices_) {
        if (notice.kind != render::RenderNoticeKind::ResourceReady &&
            notice.kind != render::RenderNoticeKind::ResourceFailed) {
            continue;
        }

        for (InspectorEntry& entry : inspectors_) {
            if (entry.state != InspectorEntryState::Pending ||
                !(entry.texture.handle == notice.handle)) {
                continue;
            }

            entry.state = notice.kind == render::RenderNoticeKind::ResourceReady
                              ? InspectorEntryState::Ready
                              : InspectorEntryState::Failed;
            entry.error = notice.error;
            if (entry.state == InspectorEntryState::Failed) {
                Log::Warning(LogSandboxScreen, "Texture {Name} could not be uploaded: {Error}",
                             StringView(entry.name), ErrorCodeName(notice.error));
            }
        }
    }
}

Result<uint32> ScreenHost::AppendEntry(StringView name, const InspectorTexture& texture,
                                       InspectorEntryState state) noexcept {
    InspectorEntry entry;
    CopyName(name, entry.name);
    entry.texture = texture;
    entry.state = state;
    if (auto pushed = inspectors_.TryPushBack(entry); !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(inspectors_.Size() - 1));
}

Result<uint32> ScreenHost::AddTexture(StringView name, const TexturePlatformData& data,
                                      PlatformBulkReader reader, void* context, bool cpuPlaceholder,
                                      render::CookedTextureIssue* issue) noexcept {
    if (issue != nullptr) {
        *issue = render::CookedTextureIssue::None;
    }

    if (!started_) {
        return Result<uint32>::Failure(ErrorCode::NotReady);
    }

    InspectorTexture texture;
    if (cpuPlaceholder) {
        texture.cpuPlaceholder = true;
        return AppendEntry(name, texture, InspectorEntryState::Placeholder);
    }

    PlatformTextureSource source(allocator_);
    if (auto made = MakePlatformTextureSource(data, reader, context, source); !made) {
        return Result<uint32>::Failure(made.Error());
    }

    auto handle = lane_.ReserveHandle(render::RenderResourceKind::Texture);
    if (!handle) {
        return Result<uint32>::Failure(handle.Error());
    }

    auto upload =
        render::PrepareCookedTexture(allocator_, source.source, device_, handle.Value(), issue);
    if (!upload) {
        (void)lane_.Retire(handle.Value());
        return Result<uint32>::Failure(upload.Error());
    }

    texture.handle = handle.Value();
    texture.dimension = source.source.dimension;
    texture.width = source.source.width;
    texture.height = source.source.height;
    texture.depth = source.source.depth;
    texture.layers = source.source.dimension == graphics::TextureDimension::TextureCubeArray
                         ? source.source.slices / graphics::CubeFaceCount
                         : (source.source.dimension == graphics::TextureDimension::Texture2DArray
                                ? source.source.slices
                                : 1u);
    texture.mips = static_cast<uint32>(source.source.mips.Size());
    DescribeInspectorTexture(source.source.format, source.swizzle, texture);
    auto appended = AppendEntry(name, texture, InspectorEntryState::Pending);
    if (!appended) {
        (void)lane_.Retire(handle.Value());
        return appended;
    }

    if (auto posted = lane_.PostResource(Move(upload.Value()), &render::CreateCookedTextureOnLane);
        !posted) {
        (void)inspectors_.TryPopBack();
        (void)lane_.Retire(handle.Value());
        return Result<uint32>::Failure(posted.Error());
    }

    return appended;
}

Result<uint32> ScreenHost::AddCachedTexture(StringView name,
                                            const TextureRenderInfo& info) noexcept {
    if (!started_) {
        return Result<uint32>::Failure(ErrorCode::NotReady);
    }

    InspectorTexture texture;
    InspectorEntryState state = InspectorEntryState::Pending;
    switch (info.state) {
    case TextureRenderState::CpuPlaceholder:
        texture.cpuPlaceholder = true;
        return AppendEntry(name, texture, InspectorEntryState::Placeholder);
    case TextureRenderState::Uploading:
        state = InspectorEntryState::Pending;
        break;
    case TextureRenderState::Ready:
        state = InspectorEntryState::Ready;
        break;
    case TextureRenderState::Failed:
        state = InspectorEntryState::Failed;
        break;
    case TextureRenderState::Loading:
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    if (info.handle.IsNull()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    texture.handle = info.handle;
    texture.dimension = info.dimension;
    texture.width = info.width;
    texture.height = info.height;
    texture.depth = info.depth;
    texture.layers =
        info.dimension == graphics::TextureDimension::TextureCubeArray
            ? info.slices / graphics::CubeFaceCount
            : (info.dimension == graphics::TextureDimension::Texture2DArray ? info.slices : 1u);
    texture.mips = info.mips;
    DescribeInspectorTexture(info.format, info.swizzle, texture);
    auto appended = AppendEntry(name, texture, state);
    if (appended) {
        InspectorEntry& entry = inspectors_[appended.Value()];
        entry.cacheOwned = true;
        entry.error = info.error;
    }

    return appended;
}

Result<uint32> ScreenHost::AddUnchecked(StringView name, render::CookedTextureUpload&& upload,
                                        const InspectorTexture& texture) noexcept {
    if (!started_) {
        return Result<uint32>::Failure(ErrorCode::NotReady);
    }

    auto handle = lane_.ReserveHandle(render::RenderResourceKind::Texture);
    if (!handle) {
        return Result<uint32>::Failure(handle.Error());
    }

    InspectorTexture entry = texture;
    entry.handle = handle.Value();
    upload.handle = handle.Value();
    auto appended = AppendEntry(name, entry, InspectorEntryState::Pending);
    if (!appended) {
        (void)lane_.Retire(handle.Value());
        return appended;
    }

    if (auto posted = lane_.PostResource(Move(upload), &render::CreateCookedTextureOnLane);
        !posted) {
        (void)inspectors_.TryPopBack();
        (void)lane_.Retire(handle.Value());
        return Result<uint32>::Failure(posted.Error());
    }

    return appended;
}

void ScreenHost::AddEngineTextures() noexcept {
    for (uint32 index = 1; index < InspectorViewCount; ++index) {
        InspectorView view = static_cast<InspectorView>(index);
        if (view == InspectorView::CubeArray && !device_.Caps().features.textureCubeArray) {
            continue;
        }

        MemoryPlatformTexture memory(allocator_);
        render::CookedTextureIssue issue = render::CookedTextureIssue::None;
        auto built = memory.Build(EngineSpec(view), &EngineTexel, &view);
        auto added = built ? AddTexture(EngineTextureNames[index], memory.Data(),
                                        &MemoryPlatformTexture::ReadBulk, &memory, false, &issue)
                           : Result<uint32>::Failure(built.Error());
        if (!added) {
            Log::Warning(LogSandboxScreen,
                         "Engine texture {Name} could not be uploaded: {Error} ({Issue})",
                         StringView(EngineTextureNames[index]), ErrorCodeName(added.Error()),
                         render::CookedTextureIssueName(issue));
            continue;
        }

        engineEntries_[index] = added.Value();
    }
}

void ScreenHost::HandleInspectorInput(const input::InputState& input) noexcept {
    constexpr input::Key ViewKeys[InspectorViewCount] = {
        input::Key::Digit0, input::Key::Digit1, input::Key::Digit2, input::Key::Digit3,
        input::Key::Digit4, input::Key::Digit5, input::Key::Digit6};
    constexpr uint32 FirstGamepad = 0;
    uint32 entry = selected_;
    uint32 layer = inspectorLayer_;
    uint32 mip = inspectorMip_;
    for (uint32 index = 0; index < InspectorViewCount; ++index) {
        if (input.WasPressed(ViewKeys[index])) {
            entry = EngineEntry(static_cast<InspectorView>(index));
            layer = 0;
            mip = 0;
        }
    }

    const uint32 count = InspectorEntryCount();
    if (count != 0 && (input.WasPressed(input::Key::Period) ||
                       input.WasPressed(FirstGamepad, input::GamepadButton::DPadRight))) {
        entry = entry >= count ? 0 : (entry + 1 == count ? NoInspectorEntry : entry + 1);
        layer = 0;
        mip = 0;
    }

    if (count != 0 && (input.WasPressed(input::Key::Comma) ||
                       input.WasPressed(FirstGamepad, input::GamepadButton::DPadLeft))) {
        entry = entry >= count ? count - 1 : (entry == 0 ? NoInspectorEntry : entry - 1);
        layer = 0;
        mip = 0;
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

    if (overlay_ && input.WasPressed(input::Key::F1)) {
        overlayVisible_ = !overlayVisible_;
        Log::Info(LogSandboxScreen, "Screen overlay {State}", overlayVisible_ ? "on" : "off");
    }

    if (entry != selected_ || layer != inspectorLayer_ || mip != inspectorMip_) {
        SetInspectorEntry(entry, layer, mip);
        if (selected_ < inspectors_.Size()) {
            Log::Info(LogSandboxScreen, "Texture inspector {Name} layer {Layer} mip {Mip}",
                      StringView(inspectors_[selected_].name), inspectorLayer_, inspectorMip_);
        } else {
            Log::Info(LogSandboxScreen, "Texture inspector off (scene)");
        }
    }
}

void ScreenHost::Resize(uint32 width, uint32 height) noexcept {
    if (!started_ || width == 0 || height == 0) {
        return;
    }

    width_ = width;
    height_ = height;
    // 표면 작업이다. 프레임 함수도 장면 pass 전에 packet 크기로 다시 맞춘다(18.5절 9항).
    if (!surface_.IsNull()) {
        (void)ui_.ResizeWindowSurface(surface_, width, height);
    }
}

void ScreenHost::ReleaseSurface() noexcept {
    if (!started_ || surfaceLost_) {
        return;
    }

    surfaceLost_ = true;
    // 창 표면 파괴가 그 창의 프레임을 모두 처리한 뒤 스왑체인을 Retire한다. 이어 장치 대기가 그
    // 표면을 쓰는 GPU 일과 회수를 끝낸다(5.3절 예외 1).
    auto destroyed =
        surface_.IsNull() ? Result<void>::Success() : ui_.DestroyWindowSurface(surface_);
    surface_ = {};
    // 파괴 대기가 실패해도(대기 상한) 장치 대기는 게시한다(뒤에 처리된 파괴의 GPU 일까지 끝낸다).
    auto released = lane_.PostSurfaceAndWait(SurfaceReleaseWait{}, &WaitSurfaceRelease);
    if (!destroyed) {
        released = Move(destroyed);
    }

    if (!released) {
        Log::Error(LogSandboxScreen, "The swap chain could not be released: {Error}",
                   ErrorCodeName(released.Error()));
    }
}

void ScreenHost::RecreateSurface(platform::Window& window) noexcept {
    if (!started_ || !surfaceLost_) {
        return;
    }

    if (window.Width() == 0 || window.Height() == 0) {
        return;
    }

    if (CreateWindowSurface(window)) {
        surfaceLost_ = false;
    }
}

Result<void> ScreenHost::CreateWindowSurface(platform::Window& window) noexcept {
    const platform::NativeWindowHandle native = window.NativeHandle();
    ui::UiWindowSurfaceDesc desc;
    desc.nativeWindow = native.window;
    desc.nativeInstance = native.instance;
    desc.width = window.Width();
    desc.height = window.Height();
    // 게임 창은 장치 스왑체인 기본(수직 동기화)이다(UI 창 표면 기본은 에디터 창 값이라 끔).
    desc.vsync = graphics::SwapChainDesc{}.vsync;
    auto created = ui_.CreateWindowSurface(desc);
    if (!created) {
        Log::Error(LogSandboxScreen,
                   "The window surface could not be created ({Width}x{Height}): "
                   "{Error}",
                   desc.width, desc.height, ErrorCodeName(created.Error()));
        return Result<void>::Failure(created.Error());
    }

    surface_ = created.Value();
    width_ = desc.width;
    height_ = desc.height;
    return Result<void>::Success();
}

Result<ui::UiFramePacket> ScreenHost::RecordOverlay(FrameArena& arena) noexcept {
    OverlayState state;
    const uint32 count = InspectorEntryCount();
    if (selected_ < count) {
        const InspectorTexture& texture = inspectors_[selected_].texture;
        const uint32 mips = texture.mips != 0 ? texture.mips : 1;
        const uint32 layers = InspectorLayerCount(texture, inspectorMip_);
        state.entry = static_cast<float>(selected_ + 1) / static_cast<float>(count);
        state.mip = static_cast<float>(inspectorMip_ + 1) / static_cast<float>(mips);
        state.layer =
            static_cast<float>(inspectorLayer_ + 1) / static_cast<float>(layers != 0 ? layers : 1);
    }

    auto buffer = ui_.AcquireDrawBuffer();
    if (!buffer) {
        return Result<ui::UiFramePacket>::Failure(buffer.Error());
    }

    auto added = overlay_->Record(state, surface_, width_, height_, *buffer.Value());
    auto packet = added ? ui_.RecordWindows(arena, *buffer.Value())
                        : Result<ui::UiFramePacket>::Failure(added.Error());
    ui_.ReleaseDrawBuffer(*buffer.Value());
    return packet;
}

Result<void> ScreenHost::RequestCapture() noexcept {
    if (!started_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto handle = lane_.ReserveHandle(render::RenderResourceKind::Texture);
    if (!handle) {
        return Result<void>::Failure(handle.Error());
    }

    capture_ = handle.Value();
    return lane_.PostResource(CaptureCreate{capture_, width_, height_}, &CreateCapture);
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

    if (!inspectorsRetired_) {
        inspectorsRetired_ = true;
        for (const InspectorEntry& entry : inspectors_) {
            // 캐시 handle은 캐시 Shutdown(엔진 종료 hook BeginShutdown)이 Retire했다.
            if (!entry.texture.handle.IsNull() && !entry.cacheOwned) {
                (void)lane_.Retire(entry.texture.handle);
            }
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
