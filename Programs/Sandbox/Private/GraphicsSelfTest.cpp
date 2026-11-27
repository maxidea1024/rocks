#include "GraphicsSelfTest.h"
#include "BufferSelfTest.h"
#include "CommandSelfTest.h"
#include "ComputeSelfTest.h"
#include "ConstantSelfTest.h"
#include "CookSamplingSelfTest.h"
#include "ShaderParameterSelfTest.h"
#include "ShaderResourceSelfTest.h"
#include "MultiTextureSelfTest.h"
#include "CullingSelfTest.h"
#include "EngineShaderSelfTest.h"
#include "RenderGraphSelfTest.h"
#include "RenderLaneSelfTest.h"
#include "RenderUtilitySelfTest.h"
#include "ScreenSelfTest.h"
#include "TextureSceneSelfTest.h"
#include "ShaderReloadSelfTest.h"
#include "TimingSelfTest.h"
#include "UploadSelfTest.h"
#include "UtilityPassSelfTest.h"
#include "FormatSelfTest.h"
#include "TextureSelfTest.h"
#include "MsaaSelfTest.h"
#include "SliceAttachmentSelfTest.h"
#include "StorageTargetSelfTest.h"
#include "SwapChainShareSelfTest.h"
#include "ViewDimensionSelfTest.h"
#include "TextureViewSelfTest.h"
#include "TransientHeapSelfTest.h"
#include "VertexSelfTest.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <D3D11Graphics/D3D11Device.h>
#include <D3D12Graphics/D3D12Device.h>
#include <Graphics/GpuFenceEvent.h>
#include <Graphics/GraphicsLog.h>
#if SANDBOX_OPENGL
#include <OpenGLGraphics/OpenGLDevice.h>
#endif
#include <PlatformWindow/Window.h>
#include <VulkanGraphics/VulkanDevice.h>
#include "SampleRenderer.h"
#include "SandboxShaders.h"
#include "SelfTestEnvironment.h"
#include <stdio.h>
#include <Platform/Memory.h>

namespace sandbox {

namespace {

using namespace rocks;

constexpr uint32 Size = 64;

// 로컬 Y(오른쪽)·Z(위)를 clip X(오른쪽)·Y(위)로 보내는 변환이다(+X를 바라보는 정면, UE 좌표).
// 단위 메시가 화면을 가득 채운다.
Mat4 FillScreen() noexcept {
    Mat4 matrix = Mat4::Zero();
    matrix.m[4] = 2.0f;  // y → clip x
    matrix.m[9] = 2.0f;  // z → clip y
    matrix.m[14] = 0.5f; // clip z
    matrix.m[15] = 1.0f; // w
    return matrix;
}

struct Pixel {
    uint8 r;
    uint8 g;
    uint8 b;
    uint8 a;
};

Pixel At(const Array<uint8>& pixels, uint32 x, uint32 y) noexcept {
    const usize offset = (static_cast<usize>(y) * Size + x) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

bool Dominant(Pixel pixel, int channel) noexcept {
    const int values[3] = {pixel.r, pixel.g, pixel.b};
    for (int index = 0; index < 3; ++index) {
        if (index != channel && values[index] + 60 > values[channel]) {
            return false;
        }
    }
    return true;
}

struct BackendResult {
    bool ran = false;
    bool passed = false;
    Array<uint8> triangle;
    Array<uint8> quad;
    // quad 결과(RenderTarget)를 텍스처로 샘플링해 다른 RenderTarget에 옮긴 결과다.
    Array<uint8> copy;

    explicit BackendResult(Allocator& allocator) noexcept
        : triangle(allocator), quad(allocator), copy(allocator) {
    }
};

uint32 failures = 0;
// 엔진 셰이더 probe만 도는 실행인가(--engine-shader-self-test).
bool engineShadersOnly = false;
// 쿠킹 엔진 셰이더 라이브러리 화면 검사만 도는 실행인가(--cooked-shader-self-test, ShaderSystem
// 16.1절 SSD48).
bool cookedShadersOnly = false;
bool texturesOnly = false;
// 감시 재컴파일 검사만 도는 실행인가(--shader-reload-self-test, ShaderSystem 16.3절 SSD78).
bool shaderReloadOnly = false;
// 감시 재컴파일 검사를 돈 장치 수다(장치마다 다른 색을 고른다).
uint32 reloadDevices = 0;
// 렌더 lane 검사의 엔진이 쓰는 프로세스 모듈 로더다(RunGraphicsSelfTest 동안 유효).
ModuleLoader* selfTestModules = nullptr;

void Check(bool condition, const char* backend, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s: %s\n", backend, what);
    }
}

// 숨김 창에 스왑체인을 만들어 프레임·크기 변경·기록 중 파괴를 확인한다(화면 픽셀은 읽지 않는다).
void RunSwapChainChecks(const char* name, graphics::Device& device, sample::Renderer& renderer,
                        Allocator& allocator) noexcept {
    if (const char* reason = WindowPresentUnavailableReason(device)) {
        printf("  [NOT RUN] %s: swap chain checks (%s)\n", name, reason);
        return;
    }

    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks graphics self-test";
    windowDesc.width = 96;
    windowDesc.height = 64;
    windowDesc.visible = false;
    platform::WindowCreateReport windowReport;
    auto window = platform::Window::Create(allocator, windowDesc, &windowReport);
    input::InputQueue queue(allocator);
    if (!window || !queue.TryInitialize(64)) {
        // 창을 여는 쪽이 원인을 남긴다. 시험은 이 검사를 건너뛰고 계속하므로 Warning이다.
        if (!window) {
            Log::Warning(platform::LogPlatformWindow,
                         "Window creation failed at {Call}: {Error} (OS error {OsError}); skipping "
                         "the {Backend} swap chain checks",
                         windowReport.call, ErrorCodeName(windowReport.code), windowReport.osError,
                         name);
        }

        printf("  [NOT RUN] %s: swap chain checks (window unavailable)\n", name);
        return;
    }
    const platform::NativeWindowHandle native = window.Value()->NativeHandle();
    graphics::SwapChainDesc desc;
    desc.surface.window = native.window;
    desc.surface.instance = native.instance;
    desc.width = windowDesc.width;
    desc.height = windowDesc.height;
    desc.vsync = false;
    auto swapChain = device.CreateSwapChain(desc);
    Check(swapChain.IsOk(), name, "swap chain creation");
    if (!swapChain) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
        return;
    }
    sample::RenderView view;
    view.viewProjection = FillScreen();
    view.clear = {0.1f, 0.1f, 0.1f, 1.0f};
    sample::RenderItem item;
    item.mesh = sample::MeshKind::Triangle;
    auto frameOnce = [&]() {
        window.Value()->PumpEvents(queue);
        auto frame = device.BeginFrame(swapChain.Value(), view.clear);
        if (!frame) {
            return false;
        }
        renderer.Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
        return device.EndFrame().IsOk();
    };
    bool frames = true;
    for (int index = 0; index < 4; ++index) {
        frames = frameOnce() && frames;
    }
    Check(frames, name, "swap chain frames present");
    Check(device.ResizeSwapChain(swapChain.Value(), 128, 80).IsOk() && frameOnce() && frameOnce(),
          name, "swap chain resize and present");

    // 기록 중인 스왑체인을 파괴해도 EndFrame이 안전해야 한다(present는 생략된다).
    auto frame = device.BeginFrame(swapChain.Value(), view.clear);
    bool destroyed = frame.IsOk();
    if (frame) {
        renderer.Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
        device.Retire(swapChain.Value());
        destroyed = device.EndFrame().IsOk();
    }
    Check(destroyed && device.WaitIdle().IsOk(), name, "swap chain destroyed while recording");
    Check(device.BeginFrame(swapChain.Value(), view.clear).Error() == ErrorCode::StaleHandle, name,
          "destroyed swap chain handle is stale");
}

// 다른 스레드에서 fence 완료를 기다린 결과다.
struct FenceWaiter {
    graphics::Device* device = nullptr;
    graphics::GpuFencePoint point;
    bool signaled = false;
};

// fence 통지를 받은 결과다.
struct FenceNotified {
    platform::ManualResetEvent done;
    ErrorCode result = ErrorCode::Cancelled;
};

// GX7 제출·완료·회수(RenderBase 4.4절): EndFrame 완료 지점, 다른 스레드의 대기, 완료 통지, Core
// 작업 이벤트 연결, 다른 장치 지점 거부, fence 뒤 회수(OpenGL 포함 즉시 삭제 없음)를 확인한다.
void RunFenceChecks(const char* name, graphics::Device& device, sample::Renderer& renderer,
                    graphics::TextureHandle target, Allocator& allocator) noexcept {
    sample::RenderView view;
    view.viewProjection = FillScreen();
    sample::RenderItem item;
    item.mesh = sample::MeshKind::Triangle;
    auto frame = device.BeginOffscreen(target, view.clear);
    Check(frame.IsOk(), name, "fence frame begins");
    if (!frame) {
        return;
    }
    renderer.Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
    auto submitted = device.EndFrame();
    Check(submitted.IsOk() && !submitted.Value().IsNull() &&
              submitted.Value().deviceEpoch == device.Info().deviceEpoch &&
              device.LastSubmittedFence() == submitted.Value(),
          name, "EndFrame returns this device's fence point");
    if (!submitted) {
        return;
    }
    const graphics::GpuFencePoint point = submitted.Value();

    FenceWaiter waiter;
    waiter.device = &device;
    waiter.point = point;
    platform::Thread thread;
    const bool started = thread.Start(
                             [](void* raw) noexcept {
                                 auto* state = static_cast<FenceWaiter*>(raw);
                                 auto waited = state->device->WaitFence(state->point);
                                 state->signaled =
                                     waited && waited.Value() == platform::WaitResult::Signaled;
                             },
                             &waiter) == platform::ThreadStatus::Success;
    Check(started && thread.Join() == platform::ThreadStatus::Success && waiter.signaled, name,
          "fence wait from another thread is signaled");
    auto complete = device.IsFenceComplete(point);
    Check(complete && complete.Value(), name, "waited fence is complete");

    FenceNotified notified;
    auto registered = device.NotifyFence(
        point,
        [](ErrorCode result, void* context) noexcept {
            auto* state = static_cast<FenceNotified*>(context);
            state->result = result;
            state->done.Signal();
        },
        &notified);
    Check(registered.IsOk() && notified.done.Wait() == platform::WaitResult::Signaled &&
              notified.result == ErrorCode::None,
          name, "fence notification reports completion");

    // 이 장치가 신호하지 않은 지점은 거부한다.
    graphics::GpuFencePoint foreign = point;
    foreign.deviceEpoch += 1000;
    graphics::GpuFencePoint future = point;
    future.value += 1000;
    Check(device.IsFenceComplete(foreign).Error() == ErrorCode::WrongContext &&
              device.IsFenceComplete(future).Error() == ErrorCode::InvalidArgument,
          name, "foreign and unsubmitted fence points are rejected");

    // fence 완료를 작업 이벤트로 잇는다: 다음 제출이 끝나면 그 뒤 작업이 실행된다.
    WorkerScheduler scheduler(allocator);
    bool ran = false;
    bool eventOk = scheduler.Start().IsOk();
    if (eventOk) {
        auto again = device.BeginOffscreen(target, view.clear);
        auto nextPoint =
            again ? device.EndFrame() : Result<graphics::GpuFencePoint>::Failure(again.Error());
        auto event = scheduler.TryCreateEvent();
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [&ran](WorkerTaskContext&) noexcept { ran = true; });
        eventOk = nextPoint.IsOk() && event.IsOk() && body.IsOk();
        if (eventOk) {
            auto after = Span<const WorkerTaskHandle>(&event.Value().Handle(), 1);
            auto task = scheduler.Submit(Move(body.Value()), after);
            eventOk = task.IsOk() && graphics::TrySubmitFenceEvent(device, nextPoint.Value(),
                                                                   Move(event.Value()), allocator)
                                         .IsOk();
            if (eventOk) {
                auto waited = task.Value().Wait();
                auto status = task.Value().Status();
                eventOk = waited && status && status.Value().state == WorkerTaskState::Succeeded;
            }
        }
        eventOk = scheduler.Shutdown().IsOk() && eventOk;
    }
    Check(eventOk && ran, name, "fence completion triggers a worker task event");

    // fence 뒤 회수: Retire는 즉시 지우지 않고 회수 대기에 두며, 끝난 fence 뒤 ReclaimCompleted가
    // 지운다.
    Check(device.WaitIdle().IsOk() && device.PendingRetireCount() == 0, name,
          "nothing is pending after WaitIdle");
    const uint8 bytes[12] = {};
    graphics::BufferDesc bufferDesc;
    bufferDesc.size = sizeof(bytes);
    auto doomed = device.CreateBuffer(bufferDesc, Span<const uint8>(bytes, sizeof(bytes)));
    Check(doomed.IsOk(), name, "retire probe buffer");
    if (doomed) {
        // 초기 데이터 업로드가 아직 제출되지 않았을 수 있다(D3D12 업로드 기록). 그 제출의 fence가
        // 끝나기 전에는 회수하지 않는다.
        device.Retire(doomed.Value());
        const usize pending = device.PendingRetireCount();
        device.ReclaimCompleted();
        const bool reclaimed = device.WaitIdle().IsOk() && device.PendingRetireCount() == 0;
        Check(pending == 1 && reclaimed, name,
              "retired buffer waits for its fence then is reclaimed");
    }
}

// 소유권을 넘겨받은 스레드에서 그리는 시험의 상태다.
struct OwnerThreadRun {
    graphics::Device* device = nullptr;
    sample::Renderer* renderer = nullptr;
    graphics::TextureHandle target;
    Array<uint8>* pixels = nullptr;
    platform::ManualResetEvent acquired;
    platform::ManualResetEvent proceed;
    bool owned = false;
    bool drew = false;
    bool released = false;

    static void Run(void* raw) noexcept {
        auto* self = static_cast<OwnerThreadRun*>(raw);
        self->owned = self->device->AcquireOwnership().IsOk() && self->device->IsOwnerThread();
        self->acquired.Signal();
        self->proceed.Wait();
        if (self->owned) {
            sample::RenderView view;
            view.viewProjection = FillScreen();
            sample::RenderItem item;
            item.mesh = sample::MeshKind::Triangle;
            auto frame = self->device->BeginOffscreen(self->target, view.clear);
            if (frame) {
                self->renderer->Draw(*frame.Value(), view,
                                     Span<const sample::RenderItem>(&item, 1));
                self->drew = self->device->EndFrame().IsOk() &&
                             self->pixels->TryResize(static_cast<usize>(Size) * Size * 4).IsOk() &&
                             self->device->ReadPixels(self->target, self->pixels->View()).IsOk();
            }
        }
        self->released = self->device->ReleaseOwnership().IsOk();
    }
};

// GX11 장치 스레드 모델·GX10 소유권 이관(RenderBase 5.4절): 생성 스레드 안전 여부 공개, 소유자가
// 없을 때 소유 스레드 전용 호출 거부(Development), 다른 스레드로 넘겨 그리고 되돌려 받기를
// 확인한다.
void RunOwnershipChecks(const char* name, graphics::Device& device, sample::Renderer& renderer,
                        graphics::TextureHandle target, Allocator& allocator) noexcept {
    // D3D12·D3D11·Vulkan은 생성이 스레드 안전이고(GX8 업로드 경로), OpenGL은 소유 스레드 전용이다.
    const bool concurrent = device.Info().backend != graphics::Backend::OpenGL;
    Check(device.Info().concurrentResourceCreation == concurrent && device.IsOwnerThread(), name,
          "device starts owned by its creating thread");
    Check(device.ReleaseOwnership().IsOk() && !device.IsOwnerThread(), name,
          "owner releases the device");
    if constexpr (!IsReleaseBuild) {
        const uint64 before = device.RejectedCalls();
        const graphics::ClearColor clear;
        const bool rejected =
            device.BeginOffscreen(target, clear).Error() == ErrorCode::WrongThread &&
            device.WaitIdle().Error() == ErrorCode::WrongThread;
        device.ReclaimCompleted();
        Check(rejected && device.RejectedCalls() == before + 3, name,
              "owner-only calls are rejected while the device has no owner");
    }

    Array<uint8> pixels(allocator);
    OwnerThreadRun run;
    run.device = &device;
    run.renderer = &renderer;
    run.target = target;
    run.pixels = &pixels;
    platform::Thread thread;
    const bool started =
        thread.Start(&OwnerThreadRun::Run, &run) == platform::ThreadStatus::Success;
    if (started) {
        run.acquired.Wait();
        // 다른 스레드가 가진 동안 이 스레드는 잡거나 놓을 수 없다.
        Check(device.AcquireOwnership().Error() == ErrorCode::Busy &&
                  device.ReleaseOwnership().Error() == ErrorCode::WrongThread,
              name, "a second thread cannot take or drop an owned device");
        run.proceed.Signal();
        Check(thread.Join() == platform::ThreadStatus::Success, name, "owner thread joins");
    }
    Check(started && run.owned && run.drew && run.released, name,
          "another thread takes the device, draws and releases it");
    if (!(run.owned && run.drew && run.released)) {
        printf("    owned %d drew %d released %d: %.*s\n", run.owned ? 1 : 0, run.drew ? 1 : 0,
               run.released ? 1 : 0, static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
    }
    Check(pixels.Size() == static_cast<usize>(Size) * Size * 4 && Dominant(At(pixels, 32, 4), 0),
          name, "frame drawn on the new owner thread is correct");
    Check(device.AcquireOwnership().IsOk() && device.IsOwnerThread(), name,
          "original thread takes the device back");
}

// 검증 계층 Warning(Graphics.*.Validation)의 수를 센다.
// 메시지 ID(속성 Id)마다 몇 번 남았는지 센다.
class ValidationWarningSink final : public RuntimeLogSink {
public:
    static constexpr uint32 MaxIds = 16;

    Result<void> Write(const LogEvent& event) noexcept override {
        if (event.level != LogLevel::Warning || !event.category.EndsWith(".Validation")) {
            return Result<void>::Success();
        }

        uint64 id = NumericLimits<uint64>::Max();
        for (const LogProperty& property : event.properties) {
            if (property.name == "Id") {
                id = property.unsignedValue;
            }
        }

        for (uint32 index = 0; index < idCount; ++index) {
            if (ids[index] == id) {
                ++counts[index];
                return Result<void>::Success();
            }
        }

        if (idCount < MaxIds) {
            ids[idCount] = id;
            counts[idCount++] = 1;
        }

        return Result<void>::Success();
    }

    // 하나 이상 남았고, 모든 ID가 정확히 한 번씩인지다.
    bool EachIdOnce() const noexcept {
        bool once = idCount != 0;
        for (uint32 index = 0; index < idCount; ++index) {
            once = once && counts[index] == 1;
        }

        return once;
    }

    uint64 ids[MaxIds]{};
    uint32 counts[MaxIds]{};
    uint32 idCount = 0;
};

void RunD3D12CreateReportProbe(Allocator& allocator) noexcept;

// 한 backend에서 삼각형·사각형을 그려 픽셀을 검사한다.
void RunBackend(const char* name, Result<UniquePtr<graphics::Device>> created,
                BackendResult& result,
                const graphics::DeviceCreateReport* report = nullptr) noexcept {
    if (!created) {
        // 이 backend를 건너뛰고 다른 후보로 계속한다(대체 후보가 있으니 Warning, 모두 실패하면
        // RunGraphicsSelfTest가 Error를 남긴다).
        const bool known = report != nullptr && report->stage[0] != '\0';
        Log::Warning(graphics::LogGraphics,
                     "{Backend} device creation failed at {Stage}: {Error} (native code "
                     "{NativeCode:#x}); skipping this backend",
                     name, known ? report->stage : "unknown stage", ErrorCodeName(created.Error()),
                     static_cast<uint64>(known ? report->nativeCode : 0));
        printf("  [NOT RUN] %s: device unavailable (error %u)\n", name,
               static_cast<unsigned>(created.Error()));
        if (report != nullptr && report->stage[0] != '\0') {
            printf("    %s (native 0x%08llX): %s\n", report->stage,
                   static_cast<unsigned long long>(report->nativeCode), report->detail);
        }

        return;
    }
    result.ran = true;
    const uint32 before = failures;
    graphics::Device& device = *created.Value();
    const graphics::DeviceInfo info = device.Info();
    printf("  %s: %.*s (validation %s%s%s)\n", name, static_cast<int>(info.adapterName.ByteCount()),
           info.adapterName.Data(), info.validationEnabled ? "on" : "off",
           info.gpuValidationEnabled ? ", GPU-based validation" : "",
           info.software ? ", software" : "");
    // 쿠킹 보장 샘플링 표와 장치 능력 대조(Docs/Design/PixelFormat 7.1절)는 셰이더가 필요 없어
    // 전체 검사와 텍스처 검사에서 돈다.
    if (!engineShadersOnly && !cookedShadersOnly) {
        failures += RunCookSamplingChecks(name, device);
    }

    if (texturesOnly) {
        // 쿠킹 텍스처 장면 대조만 돈다(RenderBase 18.5절 7항).
        failures += RunTextureSceneChecks(name, device, result.triangle.GetAllocator());
        failures +=
            RunCookedTextureChecks(name, device, result.triangle.GetAllocator(), *selfTestModules);
        result.passed = failures == before;
        printf("  [%s] %s\n", result.passed ? "PASS" : "FAIL", name);
        return;
    }

    if (cookedShadersOnly) {
        // 쿠킹 엔진 라이브러리 화면 검사만 돈다(공급자 없이 — Release 실행 파일에서도 된다).
        failures +=
            RunCookedScreenChecks(name, device, result.triangle.GetAllocator(), *selfTestModules);
        result.passed = failures == before;
        printf("  [%s] %s\n", result.passed ? "PASS" : "FAIL", name);
        return;
    }

    // 장치 형식의 셰이더 집합이다(ShaderSystem 16.1절 SSD19 — Development 공급자). 열지 못하면 그
    // backend의 그리기 검사를 하지 않는다(Release 실행 파일은 공급자가 없다).
    shaders::DeviceShaders deviceShaders(result.triangle.GetAllocator(), device);
    if (!deviceShaders.IsReady()) {
        const StringView problem = deviceShaders.Problem();
        printf("  [NOT RUN] %s: shaders unavailable: %.*s\n", name,
               static_cast<int>(problem.ByteCount()), problem.Data());
        result.ran = false;
        return;
    }

    if (shaderReloadOnly) {
        failures +=
            RunShaderReloadChecks(name, device, result.triangle.GetAllocator(), reloadDevices++);
        result.passed = failures == before;
        printf("  [%s] %s\n", result.passed ? "PASS" : "FAIL", name);
        return;
    }

    // 엔진 셰이더 SL0 GPU probe(Docs/Design/EngineShaders 7.6절): CPU 기준 대조와 장치 사이 기록.
    failures += RunEngineShaderChecks(name, device, result.triangle.GetAllocator());
    // 엔진 셰이더 SL1 유틸리티 pass(Docs/Design/EngineShaders 14.6절 3항): 밉 생성·clear·기본
    // 텍스처·깊이 복사·자리표시.
    failures += RunUtilityPassChecks(name, device, result.triangle.GetAllocator());
    if (engineShadersOnly) {
        result.passed = failures == before;
        printf("  [%s] %s\n", result.passed ? "PASS" : "FAIL", name);
        return;
    }

    sample::Renderer renderer;
    auto initialized = renderer.Initialize(device, graphics::PixelFormat::RGBA8UnormSrgb);
    if (!initialized) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
        Check(false, name, "renderer initialization");
        return;
    }

    graphics::TextureDesc targetDesc;
    targetDesc.width = Size;
    targetDesc.height = Size;
    targetDesc.format = graphics::PixelFormat::RGBA8UnormSrgb;
    targetDesc.usage = graphics::TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    // 2x2 텍스처: 좌상 빨강, 우상 초록, 좌하 파랑, 우하 흰색(행 위→아래).
    const uint8 checker[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    graphics::TextureDesc checkerDesc;
    checkerDesc.width = 2;
    checkerDesc.height = 2;
    checkerDesc.sampler.filter = graphics::Filter::Bilinear;
    checkerDesc.sampler.addressU = graphics::AddressMode::Clamp;
    checkerDesc.sampler.addressV = graphics::AddressMode::Clamp;
    auto texture = device.CreateTexture(checkerDesc, Span<const uint8>(checker, sizeof(checker)));
    Check(target.IsOk() && texture.IsOk(), name, "texture creation");
    if (!target || !texture) {
        renderer.Shutdown();
        return;
    }

    sample::RenderView view;
    view.viewProjection = FillScreen();
    view.clear = {0.0f, 0.0f, 0.0f, 1.0f};
    auto drawTo = [&](graphics::TextureHandle into, sample::MeshKind mesh,
                      graphics::TextureHandle bound, Array<uint8>& out) {
        sample::RenderItem item;
        item.mesh = mesh;
        item.texture = bound;
        auto frame = device.BeginOffscreen(into, view.clear);
        if (!frame) {
            return false;
        }
        renderer.Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
        if (!device.EndFrame() || !out.TryResize(static_cast<usize>(Size) * Size * 4)) {
            return false;
        }
        return device.ReadPixels(into, out.View()).IsOk();
    };
    auto draw = [&](sample::MeshKind mesh, graphics::TextureHandle bound, Array<uint8>& out) {
        return drawTo(target.Value(), mesh, bound, out);
    };

    Check(draw(sample::MeshKind::Triangle, {}, result.triangle), name, "triangle render/readback");
    Check(draw(sample::MeshKind::Quad, texture.Value(), result.quad), name, "quad render/readback");

    // RenderTarget을 텍스처로 샘플링해도 위아래가 바뀌지 않아야 한다(GL은 원점 규약이 다르다).
    auto copyTarget = device.CreateTexture(targetDesc, {});
    Check(copyTarget.IsOk() &&
              drawTo(copyTarget.Value(), sample::MeshKind::Quad, target.Value(), result.copy),
          name, "render target sampled as a texture");
    if (result.copy.Size() == result.quad.Size()) {
        Check(Dominant(At(result.copy, 4, 4), 0), name, "sampled render target keeps top-left red");
        Check(Dominant(At(result.copy, 4, 59), 2), name,
              "sampled render target keeps bottom-left blue");
    }
    if (copyTarget) {
        device.Retire(copyTarget.Value());
    }

    // 기록 중 파괴한 텍스처는 그 프레임이 끝날 때까지 살아 있어야 한다. 파괴 뒤 업로드(별도
    // signal)나 WaitIdle이 끼어도 해제·descriptor 재사용이 일어나면 안 된다.
    auto destroyWhileRecording = [&](bool waitIdle) {
        const uint8 red[] = {255, 0, 0, 255};
        const uint8 green[] = {0, 255, 0, 255};
        graphics::TextureDesc oneDesc;
        oneDesc.width = 1;
        oneDesc.height = 1;
        auto doomed = device.CreateTexture(oneDesc, Span<const uint8>(red, 4));
        if (!doomed) {
            return false;
        }
        sample::RenderItem item;
        item.mesh = sample::MeshKind::Quad;
        item.texture = doomed.Value();
        auto frame = device.BeginOffscreen(target.Value(), view.clear);
        if (!frame) {
            device.Retire(doomed.Value());
            return false;
        }
        renderer.Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
        device.Retire(doomed.Value());
        bool ok = true;
        if (waitIdle) {
            ok = device.WaitIdle().IsOk();
        }
        auto replacement = device.CreateTexture(oneDesc, Span<const uint8>(green, 4));
        ok = device.EndFrame().IsOk() && ok && replacement.IsOk();
        Array<uint8> pixels(result.triangle.GetAllocator());
        ok = ok && pixels.TryResize(static_cast<usize>(Size) * Size * 4).IsOk() &&
             device.ReadPixels(target.Value(), pixels.View()).IsOk() &&
             Dominant(At(pixels, 32, 32), 0);
        if (replacement) {
            device.Retire(replacement.Value());
        }
        return ok;
    };
    // 텍스처별 샘플러: 최근접은 경계 직전까지 순색, Wrap은 가장자리에서 반대편과 섞이고 Clamp는
    // 순색이다.
    auto samplerProbe = [&](graphics::Filter filter, graphics::AddressMode address, uint32 x,
                            uint32 y, Pixel& out) {
        graphics::TextureDesc desc = checkerDesc;
        desc.sampler.filter = filter;
        desc.sampler.addressU = address;
        desc.sampler.addressV = address;
        auto probe = device.CreateTexture(desc, Span<const uint8>(checker, sizeof(checker)));
        Array<uint8> pixels(result.triangle.GetAllocator());
        const bool ok = probe.IsOk() && draw(sample::MeshKind::Quad, probe.Value(), pixels);
        if (probe) {
            device.Retire(probe.Value());
        }
        if (ok) {
            out = At(pixels, x, y);
        }
        return ok;
    };
    Pixel probe = {};
    Check(samplerProbe(graphics::Filter::Point, graphics::AddressMode::Clamp, 31, 4, probe) &&
              probe.r > 250 && probe.g < 5 && probe.b < 5,
          name, "nearest filter keeps texel color up to the edge");
    Check(samplerProbe(graphics::Filter::Bilinear, graphics::AddressMode::Clamp, 0, 0, probe) &&
              probe.r > 250 && probe.g < 5,
          name, "clamp address keeps the edge texel");
    Check(samplerProbe(graphics::Filter::Bilinear, graphics::AddressMode::Wrap, 0, 0, probe) &&
              probe.g > 100 && probe.b > 100,
          name, "wrap address blends with the opposite edge");
    // Border(투명 검정): 가장자리 픽셀이 테두리와 섞여 알파가 줄고 검정 배경이 비친다.
    Check(samplerProbe(graphics::Filter::Bilinear, graphics::AddressMode::Border, 0, 0, probe) &&
              probe.r > 30 && probe.r < 230 && probe.g < 5,
          name, "border address blends with the transparent border color");

    // mip: 64×64 텍스처를 4×4 픽셀 크기로 그리면 LOD 4(4×4 mip)를 읽는다. 4번 이후 mip만 파랑이다.
    {
        constexpr uint32 MipSize = 64;
        const uint32 levels = graphics::FullMipCount(MipSize, MipSize);
        Array<uint8> chain(result.triangle.GetAllocator());
        bool built = chain
                         .TryResize(static_cast<usize>(graphics::MipChainBytes(
                             graphics::PixelFormat::RGBA8UnormSrgb, MipSize, MipSize, 1, levels)))
                         .IsOk();
        usize offset = 0;
        for (uint32 level = 0; built && level < levels; ++level) {
            const uint32 extent = graphics::MipExtent(MipSize, level);
            const uint8 color[4] = {static_cast<uint8>(level >= 4 ? 0 : 255), 0,
                                    static_cast<uint8>(level >= 4 ? 255 : 0), 255};
            for (uint32 index = 0; index < extent * extent; ++index) {
                platform::MemoryCopy(chain.Data() + offset + index * 4u, color, 4);
            }
            offset += static_cast<usize>(extent) * extent * 4u;
        }
        graphics::TextureDesc mipDesc;
        mipDesc.width = MipSize;
        mipDesc.height = MipSize;
        mipDesc.mipLevels = levels;
        mipDesc.sampler.filter = graphics::Filter::Trilinear;
        auto mipped = built ? device.CreateTexture(mipDesc, chain.View())
                            : Result<graphics::TextureHandle>::Failure(ErrorCode::OutOfMemory);
        Array<uint8> pixels(result.triangle.GetAllocator());
        bool drawn = false;
        if (mipped) {
            sample::RenderItem item;
            item.mesh = sample::MeshKind::Quad;
            item.texture = mipped.Value();
            // 단위 사각형을 1/16로 줄여 64픽셀 대상에서 4×4 픽셀을 덮는다.
            item.world = Mat4::Identity();
            item.world.m[0] = 1.0f / 16.0f;
            item.world.m[5] = 1.0f / 16.0f;
            item.world.m[10] = 1.0f / 16.0f;
            auto frame = device.BeginOffscreen(target.Value(), view.clear);
            if (frame) {
                renderer.Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
                drawn = device.EndFrame().IsOk() &&
                        pixels.TryResize(static_cast<usize>(Size) * Size * 4).IsOk() &&
                        device.ReadPixels(target.Value(), pixels.View()).IsOk();
            }
            device.Retire(mipped.Value());
        }
        Check(mipped.IsOk() && drawn && Dominant(At(pixels, 31, 31), 2), name,
              "minified texture samples the matching mip level");
    }

    Check(destroyWhileRecording(false), name, "texture destroyed while recording survives uploads");
    Check(destroyWhileRecording(true), name, "texture destroyed while recording survives WaitIdle");
    if (result.triangle.Size() == static_cast<usize>(Size) * Size * 4) {
        // 위 꼭짓점 빨강, 오른쪽 아래 파랑, 왼쪽 아래 초록, 위 모서리는 배경(검정).
        Check(Dominant(At(result.triangle, 32, 4), 0), name, "triangle top is red");
        Check(Dominant(At(result.triangle, 60, 61), 2), name, "triangle bottom-right is blue");
        Check(Dominant(At(result.triangle, 3, 61), 1), name, "triangle bottom-left is green");
        const Pixel corner = At(result.triangle, 1, 1);
        Check(corner.r < 8 && corner.g < 8 && corner.b < 8, name, "triangle corner is background");
    }
    if (result.quad.Size() == static_cast<usize>(Size) * Size * 4) {
        // UV (0,0)이 좌상단: 좌상 빨강, 우상 초록, 좌하 파랑, 우하 흰색.
        Check(Dominant(At(result.quad, 4, 4), 0), name, "quad top-left samples red");
        Check(Dominant(At(result.quad, 59, 4), 1), name, "quad top-right samples green");
        Check(Dominant(At(result.quad, 4, 59), 2), name, "quad bottom-left samples blue");
        const Pixel white = At(result.quad, 59, 59);
        Check(white.r > 200 && white.g > 200 && white.b > 200, name,
              "quad bottom-right samples white");
    }
    RunFenceChecks(name, device, renderer, target.Value(), result.triangle.GetAllocator());
    RunOwnershipChecks(name, device, renderer, target.Value(), result.triangle.GetAllocator());
    RunSwapChainChecks(name, device, renderer, result.triangle.GetAllocator());
    // 좌표·앞면 규약(C7): 컬 모드·음수 행렬식 반전·행 우선 행렬을 창·offscreen 두 경로로 확인한다.
    failures += RunCullingChecks(name, device, renderer, result.triangle.GetAllocator());
    Check(device.IgnoredDraws() == 0, name, "no ignored draws");
    // 정점 배치(GX3a): 형식 20종·stream·인스턴스·index·정점 버퍼 없는 그리기와 음성 대조.
    failures += RunVertexChecks(name, device, result.triangle.GetAllocator());
    // 업로드·readback(GX8): 내용 갱신·ring wrap·staging 회수·비동기 readback·다른 스레드 생성.
    failures += RunUploadChecks(name, device, renderer, result.triangle.GetAllocator());
    // 픽셀 형식(PF2·PF3·PF6): 능력 ⇔ 생성, 업로드·readback 왕복, sRGB·깊이·스텐실·비디오 view.
    failures += RunFormatChecks(name, device, result.triangle.GetAllocator());
    // 텍스처 차원·view swizzle·샘플러 확장(AssetSystem A0-G).
    failures += RunTextureChecks(name, device, result.triangle.GetAllocator());
    // 텍스처 view 범위·쓰기 텍스처·compute 텍스처 묶기(텍스처 GX2 A부): HZB 줄이기·mip·slice view.
    failures += RunTextureViewChecks(name, device, result.triangle.GetAllocator());
    // MSAA 텍스처·attachment resolve(텍스처 GX2 B부, 결정 R17).
    failures += RunMsaaChecks(name, device, result.triangle.GetAllocator());
    // slice·mip attachment(텍스처 GX2 C부): mip 사슬·배열 층·cube 면·3D 면·깊이 층·resolve slice.
    failures += RunSliceAttachmentChecks(name, device, result.triangle.GetAllocator());
    // 용도 비트 묶음·cube Storage(텍스처 GX2 나머지 A부): 렌더 타깃 + UAV, cube 면 쓰기 view.
    failures += RunStorageTargetChecks(name, device, result.triangle.GetAllocator());
    // view 차원 바꾸기·OpenGL ES 범위 view(텍스처 GX2 나머지 B부).
    failures += RunViewDimensionChecks(name, device, result.triangle.GetAllocator());
    // 장치 능력(GX1): 한도·기능 로그, MSAA 표본 수와 Multisample 능력 일관성.
    failures += RunCapsChecks(name, device);
    // 버퍼 용도·view(GX2): 생성·업로드·readback, typed view 능력 ⇔ 생성, VS·PS의 view 읽기.
    failures += RunBufferChecks(name, device, result.triangle.GetAllocator());
    // 파이프라인 완성·명령 기록 확장(GX3·GX4): compute·간접·MRT·깊이·스텐실·복사·UAV clear.
    failures += RunCommandChecks(name, device, result.triangle.GetAllocator());
    // 렌더 그래프(RG1): 그래프 텍스처·명시 전이·culling·history export/import.
    failures += RunRenderGraphChecks(name, device, renderer, result.triangle.GetAllocator());
    // 스왑체인 공유: 한 그래프의 스왑체인 pass 둘이 백버퍼 하나에 그리고 present는 하나다.
    failures += RunSwapChainShareChecks(name, device, result.triangle.GetAllocator());
    // 렌더 쪽 compute(GX3·GX4 compute 연결): 파이프라인 캐시·그래프 버퍼 view·직접·간접 dispatch.
    failures += RunComputeChecks(name, device, result.triangle.GetAllocator());
    // 상수 버퍼 묶기(GX6, RenderBase 9.11절): handle 두 수명·기록 명령·갱신 순서·D3D12 root 예산.
    failures += RunConstantChecks(name, device, result.triangle.GetAllocator());
    // 생성 셰이더 파라미터 구조체(ShaderSystem 16.2절 SH3a): cbuffer·루트 상수를 C++ 구조체 그대로
    // 올린 픽셀.
    failures += RunShaderParameterChecks(name, device, result.triangle.GetAllocator());
    // 생성 구조체의 자원 멤버(ShaderSystem 16.2절 SH3b): 생성 표 자원 지도와 SetParameters로 묶은
    // 장치 텍스처·짝 샘플러 override·typed 버퍼.
    failures += RunShaderResourceChecks(name, device, result.triangle.GetAllocator());
    // 다중 텍스처·샘플러 slot과 자원 묶음(RR12, RenderBase 9.13절): VS·PS 텍스처 다섯, 샘플러 값
    // 지정, 공유 샘플러, 파이프라인 전환 뒤 묶음 유지.
    failures += RunMultiTextureChecks(name, device, result.triangle.GetAllocator());
    // RenderBase 그리기 유틸리티(RB6, RenderBase 11.4절): 엔진 셰이더 전체화면 복사(장치·그래프).
    failures += RunRenderUtilityChecks(name, device, result.triangle.GetAllocator());
    // transient heap·placed 자원·aliasing 획득(GX12): 같은 heap 구간의 렌더 타깃·버퍼 이어 쓰기.
    failures += RunTransientHeapChecks(name, device, result.triangle.GetAllocator());
    // GPU 계측(GX9): timestamp 쌍·slot 재사용·calibration·debug marker.
    failures += RunTimingChecks(name, device, renderer, result.triangle.GetAllocator());
    // 감시 재컴파일(ShaderSystem 16.3절 SSD78): 시험 루트 원문을 고쳐 쓰고 교체·픽셀·실패 유지.
    // 활성 집합을 바꾸므로 그 집합을 쓰는 검사들 뒤에 둔다.
    failures +=
        RunShaderReloadChecks(name, device, result.triangle.GetAllocator(), reloadDevices++);
    Check(device.ValidationErrorCount() == 0, name, "no validation errors");

    // 검증 오류 집계가 실제로 동작하는지 확인한다: 파이프라인 대상 형식과 다른 대상에 그리면
    // D3D12는 debug layer(RTV 형식), Vulkan은 validation layer(render pass 호환성) 오류다.
    // OpenGL에는 대응 오류가 없고, D3D11은 파이프라인에 RTV 형식이 없어 따로
    // 확인한다(RunD3D11ValidationProbe).
    if ((info.backend == graphics::Backend::D3D12 || info.backend == graphics::Backend::Vulkan) &&
        info.validationEnabled) {
        // 이 아래 검증 계층 메시지(D3D12 613, Vulkan render pass 호환성 VUID)는 의도한 것이다.
        // 로그만 보고 실패로 오해하지 않도록 앞뒤를 표시한다.
        printf("    -- begin intentional validation messages (RTV format mismatch probe) --\n");
        fflush(stdout);
        const uint64 before = device.ValidationErrorCount();
        sample::Renderer mismatched;
        if (mismatched.Initialize(device, graphics::PixelFormat::RGBA8Unorm)) {
            sample::RenderItem item;
            item.mesh = sample::MeshKind::Triangle;
            auto frame = device.BeginOffscreen(target.Value(), view.clear);
            if (frame) {
                mismatched.Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
                auto ended = device.EndFrame();
                (void)ended;
            }
            mismatched.Shutdown();
        }
        fflush(stderr);
        printf("    -- end intentional validation messages --\n");
        Check(device.ValidationErrorCount() > before, name,
              "validation layer error is counted (intentional RTV format mismatch)");
    }

    device.Retire(texture.Value());
    device.Retire(target.Value());
    renderer.Shutdown();
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), name, "wait idle");
    if (!idle) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
    }
    // 렌더 lane(RB1): 장치를 lane으로 넘겨 엔진 루프 프레임을 그리고 되돌려 받는다.
    failures += RunRenderLaneChecks(name, device, result.triangle.GetAllocator(), *selfTestModules);
    // 엔진 창 렌더 경로(RenderBase 18.5절 SM1–SM5): 엔진 장면 → tone map → 스왑체인, CPU 기준 대조.
    failures += RunScreenChecks(name, device, result.triangle.GetAllocator(), *selfTestModules);
    // 쿠킹 텍스처 업로드 경로의 장면 대조(RenderBase 18.5절 7항 SM7 — 메모리 플랫폼 데이터).
    failures += RunTextureSceneChecks(name, device, result.triangle.GetAllocator());
    // 시험 콘텐츠 쿠킹의 텍스처(쿠킹이 없으면 NOT RUN).
    failures +=
        RunCookedTextureChecks(name, device, result.triangle.GetAllocator(), *selfTestModules);
    // 같은 화면 경로를 쿠킹 엔진 셰이더 라이브러리로(SSD48 — 엔진 쿠킹 결과가 없으면 NOT RUN).
    failures +=
        RunCookedScreenChecks(name, device, result.triangle.GetAllocator(), *selfTestModules);
    result.passed = failures == before;
    printf("  [%s] %s\n", result.passed ? "PASS" : "FAIL", name);
}

// D3D11 검증 계층 집계가 실제로 동작하는지 확인한다. D3D11은 파이프라인에 RTV 형식이 없으므로 정점
// 용도 버퍼를 index 버퍼로 묶는다(D3D11 버퍼는 용도별 bind flag로 만든다). 이 오류는 WARP 장치를
// 제거하므로 검사 장치와 따로 만든 단기 장치에서 한다.
void RunD3D11ValidationProbe(const char* name,
                             Result<UniquePtr<graphics::Device>> created) noexcept {
    if (!created || !created.Value()->Info().validationEnabled) {
        return;
    }

    graphics::Device& device = *created.Value();
    graphics::TextureDesc targetDesc;
    targetDesc.width = Size;
    targetDesc.height = Size;
    targetDesc.usage = graphics::TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    const uint8 bytes[16] = {};
    graphics::BufferDesc vertexDesc;
    vertexDesc.size = sizeof(bytes);
    auto vertexOnly = device.CreateBuffer(vertexDesc, Span<const uint8>(bytes, sizeof(bytes)));
    // 같은 메시지 ID는 세기는 매번, Warning은 한 번이다. 이 검사 동안만 기록을 모으는 LogSystem을
    // 설치한다(Sandbox 본체는 아직 설치하지 않는다, LG8).
    ValidationWarningSink sink;
    SystemAllocator logAllocator;
    LogSystemConfig logConfig = LogSystemConfig::ForTool();
    logConfig.globalMinimum = LogLevel::Warning;
    LogSystem logs(logAllocator, logConfig);
    const bool capturing = logs.AddSink(sink).IsOk() && logs.Start().IsOk();
    const uint64 before = device.ValidationErrorCount();
    {
        LogInstallScope installed(logs);
        for (uint32 attempt = 0; attempt < 2 && target && vertexOnly; ++attempt) {
            auto frame = device.BeginOffscreen(target.Value(), {});
            if (frame) {
                frame.Value()->SetIndexBuffer(vertexOnly.Value(), graphics::IndexFormat::UInt16);
                auto ended = device.EndFrame();
                (void)ended;
            }

            (void)device.ValidationErrorCount();
        }
    }

    logs.Stop();
    Check(device.ValidationErrorCount() > before, name,
          "validation layer error is counted (intentional index buffer bind flag mismatch, "
          "twice)");
    Check(capturing && sink.EachIdOnce(), name,
          "validation layer message is logged once per message ID (Graphics.D3D11.Validation)");
}

// D3D12 생성 실패가 report에 단계·원인을 남긴다. 업로드 ring을 장치가 만들 수 없을 만큼 크게 잡아
// 생성 중간에 실패시킨다(WARP라 하드웨어와 무관하다).
void RunD3D12CreateReportProbe(Allocator& allocator) noexcept {
    graphics::DeviceDesc desc;
    desc.preferSoftware = true;
    desc.uploadRingBytes = uint64{1} << 46;
    graphics::DeviceCreateReport report;
    auto created = graphics::CreateD3D12Device(allocator, desc, &report);
    if (created || created.Error() == ErrorCode::Unsupported) {
        printf("  [NOT RUN] D3D12 create report probe (device %s)\n",
               created ? "was created" : "unsupported");
        return;
    }

    Check(report.code == created.Error() && report.stage[0] != '\0' && report.detail[0] != '\0' &&
              report.nativeCode != 0,
          "D3D12", "creation failure fills DeviceCreateReport (stage, HRESULT, detail, code)");
    printf("    D3D12 forced creation failure: %s (native 0x%08llX): %s\n", report.stage,
           static_cast<unsigned long long>(report.nativeCode), report.detail);
}

// 두 backend의 결과가 픽셀 단위로 거의 같은지 본다(래스터화·필터링 차이 허용 범위 안).
void Compare(const BackendResult& left, const BackendResult& right, const char* name) noexcept {
    if (!left.ran || !right.ran || left.triangle.Size() != right.triangle.Size() ||
        left.quad.Size() != right.quad.Size()) {
        return;
    }
    uint32 different = 0;
    for (usize index = 0; index < left.quad.Size(); ++index) {
        const int delta = static_cast<int>(left.quad[index]) - static_cast<int>(right.quad[index]);
        const int triangleDelta =
            static_cast<int>(left.triangle[index]) - static_cast<int>(right.triangle[index]);
        int copyDelta = 0;
        if (left.copy.Size() == left.quad.Size() && right.copy.Size() == right.quad.Size()) {
            copyDelta = static_cast<int>(left.copy[index]) - static_cast<int>(right.copy[index]);
        }
        if (delta > 8 || delta < -8 || triangleDelta > 8 || triangleDelta < -8 || copyDelta > 8 ||
            copyDelta < -8) {
            ++different;
        }
    }
    // 가장자리 픽셀의 커버리지 판정 차이만 허용한다(전체의 2% 미만).
    const bool same = different * 50 < static_cast<uint32>(left.quad.Size());
    Check(same, name, "backends produce the same image");
    printf("  [%s] %s (%u differing channels)\n", same ? "PASS" : "FAIL", name, different);
}

// Vulkan 하드웨어와 소프트웨어 ICD(lavapipe·SwiftShader, 측정 S4)를 돌린다. 셰이더는 다른 backend와
// 같이 공급자가 만든 쿠킹 SPIR-V다(ShaderSystem 16.1절 SH2e2).
void RunVulkan(Allocator& allocator, const graphics::DeviceDesc& base, BackendResult& hardware,
               BackendResult& software) noexcept {
    graphics::DeviceCreateReport report;
    RunBackend("Vulkan", graphics::CreateVulkanDevice(allocator, base, &report), hardware, &report);
    graphics::DeviceDesc desc = base;
    desc.preferSoftware = true;
    RunBackend("Vulkan software", graphics::CreateVulkanDevice(allocator, desc, &report), software,
               &report);
}

} // namespace

int RunGraphicsSelfTest(Allocator& allocator, ModuleLoader& modules, bool gpuValidation,
                        GraphicsSelfTestScope scope) noexcept {
    selfTestModules = &modules;
    engineShadersOnly = scope == GraphicsSelfTestScope::EngineShaders;
    cookedShadersOnly = scope == GraphicsSelfTestScope::CookedShaders;
    texturesOnly = scope == GraphicsSelfTestScope::Textures;
    shaderReloadOnly = scope == GraphicsSelfTestScope::ShaderReload;
    reloadDevices = 0;
    ResetEngineShaderRecords();
    // 검증 계층이 프로세스를 끝내도 그때까지의 진행이 남도록 출력 버퍼를 쓰지 않는다.
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Graphics self-test\n");
    failures = 0;
    // 엔진 셰이더 호스트(RenderBase 9.12절): Sandbox 타입·원문 디렉터리를 넣어 서비스로 올리고 엔진
    // 묶음(RenderBase 엔진 타입·Development 공급자 모듈)을 시작한 뒤 Freeze한다. 모든 backend
    // 검사가 끝날 때까지 산다.
    shaders::SelfTestShaderHost shaderHost(modules);
    Check(shaderHost.IsReady(), "Sandbox",
          "the engine shader host registers, starts the engine modules and freezes");
    graphics::DeviceDesc desc;
    desc.enableValidation = true;
    // D3D12만 쓰고 다른 backend는 무시한다.
    desc.enableGpuValidation = gpuValidation;
    BackendResult d3d12(allocator);
    BackendResult warp(allocator);
    BackendResult d3d11(allocator);
    BackendResult d3d11Warp(allocator);
    BackendResult glesProfile(allocator);
    BackendResult glesDesktop(allocator);
    BackendResult vulkan(allocator);
    BackendResult vulkanSoftware(allocator);
    graphics::DeviceCreateReport report;
    RunBackend("D3D12", graphics::CreateD3D12Device(allocator, desc, &report), d3d12, &report);
    graphics::DeviceDesc software = desc;
    software.preferSoftware = true;
    RunBackend("D3D12 WARP", graphics::CreateD3D12Device(allocator, software, &report), warp,
               &report);
    if (!engineShadersOnly && !texturesOnly) {
        RunD3D12CreateReportProbe(allocator);
    }

    RunBackend("D3D11", graphics::CreateD3D11Device(allocator, desc, &report), d3d11, &report);
    RunBackend("D3D11 WARP", graphics::CreateD3D11Device(allocator, software, &report), d3d11Warp,
               &report);
    if (!engineShadersOnly && !texturesOnly) {
        RunD3D11ValidationProbe("D3D11", graphics::CreateD3D11Device(allocator, desc));
        RunD3D11ValidationProbe("D3D11 WARP", graphics::CreateD3D11Device(allocator, software));
    }

    // OpenGL ES 3.2 미리보기(GLES31_PC)를 두 context에서 돌린다: 진짜 ES 3.2 context(드라이버가 ES
    // 밖 호출을 거부)와 UE 방식의 데스크톱 GL 4.3 core context.
#if SANDBOX_OPENGL
    graphics::OpenGLDeviceOptions esProfile;
    esProfile.contextProfile = graphics::OpenGLContextProfile::EsProfile;
    RunBackend("OpenGL ES", graphics::CreateOpenGLDevice(allocator, desc, esProfile, &report),
               glesProfile, &report);
    graphics::OpenGLDeviceOptions desktopCore;
    desktopCore.contextProfile = graphics::OpenGLContextProfile::DesktopCore;
    RunBackend("OpenGL ES on GL core",
               graphics::CreateOpenGLDevice(allocator, desc, desktopCore, &report), glesDesktop,
               &report);
#else
    printf("  [NOT RUN] OpenGL ES: the GLES31_PC preview is kept out of Release Windows "
           "executables\n");
#endif
    RunVulkan(allocator, desc, vulkan, vulkanSoftware);
    // 엔진 셰이더 probe의 장치 사이 대조(Docs/Design/EngineShaders 8.5절)와 coverage 사유.
    failures += ReportEngineShaderCrossDevice();
    if (engineShadersOnly || cookedShadersOnly || texturesOnly || shaderReloadOnly) {
        const bool ranAny = d3d12.ran || warp.ran || d3d11.ran || d3d11Warp.ran ||
                            glesProfile.ran || glesDesktop.ran || vulkan.ran || vulkanSoftware.ran;
        printf("%s self-test: %s (%u failure%s)\n",
               engineShadersOnly   ? "Engine shader"
               : cookedShadersOnly ? "Cooked shader"
               : shaderReloadOnly  ? "Shader reload"
                                   : "Texture",
               failures == 0 && ranAny ? "PASS" : (ranAny ? "FAIL" : "NOT RUN"), failures,
               failures == 1 ? "" : "s");
        return failures == 0 ? 0 : 1;
    }

    // 카메라 입력(RenderBase 18.5절 4항): 헤드리스 엔진에 W·오른쪽 화살표를 넣는다.
    failures += RunPawnInputChecks(allocator, modules);
    Compare(d3d12, glesProfile, "D3D12 vs OpenGL ES");
    Compare(d3d12, glesDesktop, "D3D12 vs OpenGL ES on GL core");
    Compare(d3d12, warp, "D3D12 vs WARP");
    Compare(d3d12, d3d11, "D3D12 vs D3D11");
    Compare(d3d12, d3d11Warp, "D3D12 vs D3D11 WARP");
    Compare(warp, d3d11Warp, "D3D12 WARP vs D3D11 WARP");
    Compare(d3d12, vulkan, "D3D12 vs Vulkan");
    Compare(d3d12, vulkanSoftware, "D3D12 vs Vulkan software");
    const bool anyRan = d3d12.ran || warp.ran || d3d11.ran || d3d11Warp.ran || glesProfile.ran ||
                        glesDesktop.ran || vulkan.ran || vulkanSoftware.ran;
    if (!anyRan) {
        Log::Error(graphics::LogGraphics,
                   "No graphics backend could create a device (D3D12, D3D11, OpenGL ES, Vulkan and "
                   "their software fallbacks all failed); see the warnings above for each stage");
    }
    printf("Graphics self-test: %s (%u failure%s)\n",
           failures == 0 && anyRan ? "PASS" : (anyRan ? "FAIL" : "NOT RUN"), failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

} // namespace sandbox
