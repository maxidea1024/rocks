#include "TextureSceneSelfTest.h"

#include "CookedTextureLibrary.h"
#include "SandboxGame.h"
#include "SandboxShaders.h"
#include "ScreenHost.h"
#include "SelfTestEnvironment.h"
#include "TextureSceneCapture.h"

#include <Collision/CollisionSettings.h>
#include <Config/ConfigContext.h>
#include <HostConfig/HostSettings.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Engine/Engine.h>
#include <EngineAssets/EngineAssetsSubsystem.h>
#include <Platform/File.h>
#include <PlatformWindow/Window.h>
#include <stdio.h>

// 쿠킹 텍스처 장면 대조다(Docs/Design/RenderBase 18.5절 7항 (나)): 시험 콘텐츠 쿠킹을 엔진 에셋
// 런타임으로 요청하고 패키지 공개 뒤 올려 표시한다.
namespace sandbox {

namespace {

using namespace rocks;

// 시험 콘텐츠 프로젝트의 엔진 디렉터리 기준 상대 경로와 쿠킹 루트 아래 경로다.
constexpr char TestContentFromEngine[] = "/../Programs/Sandbox/TestContent/Saved/Cooked/";
// 엔진 프레임 상한이다(준비 gate가 먼저 끝낸다 — 넘으면 실패).
constexpr uint64 MaxCookedFrames = 4096;
constexpr double FrameSeconds = 1.0 / 60.0;
// 엔진 디렉터리 경로 버퍼 byte 수다.
constexpr usize MaxPathBytes = 4096;

uint32 cookedFailures = 0;
// 쿠킹 텍스처의 형식·swizzle·mip 수를 처음 돈 장치에서 한 번 찍었는가(쿠킹 결과 기록 — 에셋 빌드가
// 고른 형식을 시험 출력에 남긴다).
bool cookedFormatsListed = false;

void CookedCheck(bool condition, const char* device, const char* what) noexcept {
    if (!condition) {
        ++cookedFailures;
        printf("  [FAIL] %s cooked textures: %s\n", device, what);
    }
}

// 쿠킹 루트(<엔진>/../Programs/Sandbox/TestContent/Saved/Cooked/<프로필>)를 찾는다. 매니페스트가
// 없으면 false다.
bool FindTestContentCook(String& root) noexcept {
    char engine[MaxPathBytes];
    auto found = FindEngineDirectory(Span<char>(engine));
    if (!found) {
        return false;
    }

    root.Clear();
    if (!root.TryAppend(StringView(engine, found.Value())) ||
        !root.TryAppend(TestContentFromEngine) ||
        !root.TryAppend(ResolveRuntimeProfile(StringView()))) {
        return false;
    }

    String manifest(root.GetAllocator());
    if (!manifest.TryAppend(root.View()) || !manifest.TryAppend("/") ||
        !manifest.TryAppend(CookedManifestFileName)) {
        return false;
    }

    bool exists = false;
    return platform::PhysicalFileSystem().FileExists(manifest.CStr(), manifest.View().ByteCount(),
                                                     exists) &&
           exists;
}

// 텍스처 하나의 표시 프레임 둘이다: mip 0(capture 자리 = 요청 번호)과 가장 작은 mip(자리 = 요청 수
// + 요청 번호 — 블록 압축 형식의 4보다 작은 mip, cube 계열은 2 × 2, 개정 63). mip이 하나뿐이거나
// CPU 대체면 둘째는 없다.
constexpr uint32 FramesPerTexture = 2;

// cube 계열은 1 × 1 mip 대신 2 × 2 mip이다: 1 × 1 면은 표본점이 면 가운데에서 조금만 벗어나도
// 이음매 없는 cube 필터(D3D11 이후·Vulkan·GLES 모두)가 이웃 면 texel을 섞는다(첫 실행 — 모든 장치가
// 같은 값으로 약 1% 섞였다). 2 × 2 면의 가운데는 그 면의 네 texel만 읽는다.
constexpr uint32 CubeSmallestMipFromEnd = 2;

uint32 SmallestMip(const screen::InspectorTexture& texture) noexcept {
    if (texture.mips == 0) {
        return 0;
    }

    if (graphics::IsCubeDimension(texture.dimension)) {
        return texture.mips >= CubeSmallestMipFromEnd ? texture.mips - CubeSmallestMipFromEnd : 0u;
    }

    return texture.mips - 1;
}

// 엔진 루프를 화면과 쿠킹 텍스처 목록에 잇는다. 모든 텍스처가 정해지면(올렸거나 실패) 텍스처마다
// 표시 프레임(mip 0·가장 작은 mip)을 차례로 그리고 끝낸다(준비 gate — 프레임 수에 기대지 않는다).
class CookedTextureHooks final : public EngineLoopHooks {
public:
    CookedTextureHooks(screen::ScreenHost& screen, platform::Window& window,
                       input::InputQueue& queue) noexcept
        : screen_(screen), window_(window), queue_(queue) {
    }

    void Attach(CookedTextureLibrary* library) noexcept {
        library_ = library;
    }

    render::RenderLanePump Pump() noexcept {
        return {&PumpWindow, this};
    }

    void PumpEvents(Engine&) noexcept override {
        window_.PumpEvents(queue_);
        queue_.Clear();
    }

    void EndFrame(Engine& engine) noexcept override {
        if (library_ == nullptr) {
            engine.RequestExit();
            return;
        }

        library_->Update();
        if (!captureRequested) {
            captureRequested = screen_.RequestCapture().IsOk();
        }

        // 올린 텍스처의 업로드 통지가 모두 온 뒤 차례로 그린다.
        if (library_->IsSettled() && screen_.PendingUploads() == 0) {
            const uint32 requested = library_->Requested();
            const uint32 frames = requested * FramesPerTexture;
            while (next < frames && !HasFrame(next)) {
                ++next;
            }

            if (next >= frames || next >= screen::MaxCaptureSlots) {
                done = true;
                engine.RequestExit();
                return;
            }

            const uint32 request = next % requested;
            const uint32 entry = library_->EntryOf(request);
            const uint32 mip =
                next < requested ? 0u : SmallestMip(screen_.InspectorEntryAt(entry).texture);
            screen_.SetInspectorEntry(entry, 0, mip);
            screen_.SetCaptureSlot(next);
            ++next;
        }

        screen_.SubmitFrame(nullptr);
    }

    void BeginShutdown(Engine&) noexcept override {
        if (library_ != nullptr) {
            library_->Release();
        }

        screen_.BlockFrames();
    }

    Result<void> CompleteInFlightWork(Engine&) noexcept override {
        return screen_.WaitFramesIdle();
    }

    Result<uint64> DrainShutdownCleanup(Engine&) noexcept override {
        return screen_.DrainRetires();
    }

    Result<void> VerifyShutdownClosure(Engine&) noexcept override {
        return screen_.CloseResources();
    }

    Result<void> StopExecutors(Engine&) noexcept override {
        return screen_.Stop();
    }

    bool captureRequested = false;
    bool done = false;
    uint32 next = 0;

private:
    // frame(요청 번호 + 요청 수 × 0·1)을 그릴 것인가.
    bool HasFrame(uint32 frame) const noexcept {
        const uint32 requested = library_->Requested();
        const uint32 entry = library_->EntryOf(frame % requested);
        if (entry == screen::ScreenHost::NoInspectorEntry) {
            return false;
        }

        const screen::InspectorTexture& texture = screen_.InspectorEntryAt(entry).texture;
        return frame < requested || (!texture.cpuPlaceholder && SmallestMip(texture) != 0);
    }

    static void PumpWindow(void* raw) noexcept {
        auto* self = static_cast<CookedTextureHooks*>(raw);
        self->window_.PumpEvents(self->queue_);
    }

    screen::ScreenHost& screen_;
    platform::Window& window_;
    input::InputQueue& queue_;
    CookedTextureLibrary* library_ = nullptr;
};

// CPU 기준 계산 문맥이다(쿠킹 데이터 방문 동안).
struct ExpectationContext {
    const screen::InspectorTexture* texture = nullptr;
    uint32 request = 0;
    uint32 requested = 0;
    const char* path = nullptr;
    Array<SceneCheck>* checks = nullptr;
};

void AppendMipChecks(const ExpectationContext& context, const TexturePlatformData& data,
                     PlatformBulkReader reader, void* readerContext, graphics::PixelFormat format,
                     uint32 level, uint32 frame, const char* name) noexcept {
    auto bytes = MipBytes(data, reader, readerContext, level);
    if (!bytes) {
        return;
    }

    const TextureMipData& mip = data.mips[level];
    AppendInspectorChecks(*context.texture, 0, level, mip.sizeX, mip.sizeY, data.numSlices,
                          mip.sizeZ, format, bytes.Value(), frame, frame, name, *context.checks);
}

void ComputeExpectation(void* raw, const TexturePlatformData& data, PlatformBulkReader reader,
                        void* readerContext) noexcept {
    auto& context = *static_cast<ExpectationContext*>(raw);
    auto format = graphics::PixelFormatFromNumber(data.pixelFormat);
    if (!format || data.mips.IsEmpty()) {
        return;
    }

    if (!cookedFormatsListed) {
        constexpr char Sources[] = "RGBA01";
        const TextureSwizzle swizzles[4] = {data.swizzleR, data.swizzleG, data.swizzleB,
                                            data.swizzleA};
        char swizzle[5] = {};
        for (uint32 channel = 0; channel < 4; ++channel) {
            const uint32 source = static_cast<uint32>(swizzles[channel]);
            swizzle[channel] = source < sizeof(Sources) - 1 ? Sources[source] : '?';
        }

        printf("  cooked texture %s: %s swizzle %s, %u x %u, %u mips\n", context.path,
               graphics::PixelFormatName(format.Value()), swizzle, data.sizeX, data.sizeY,
               static_cast<uint32>(data.mips.Size()));
    }

    AppendMipChecks(context, data, reader, readerContext, format.Value(), 0, context.request,
                    context.path);
    const uint32 smallest = SmallestMip(*context.texture);
    if (!context.texture->cpuPlaceholder && smallest != 0 && smallest < data.mips.Size()) {
        char name[MaxSceneCheckName];
        snprintf(name, sizeof(name), "%s mip %u (%u x %u)", context.path, smallest,
                 data.mips[smallest].sizeX, data.mips[smallest].sizeY);
        AppendMipChecks(context, data, reader, readerContext, format.Value(), smallest,
                        context.requested + context.request, name);
    }
}

bool RunMode(const char* name, render::RenderLaneMode mode, graphics::Device& device,
             Allocator& allocator, ModuleLoader& modules, StringView cookedRoot,
             Array<SceneCheck>& checks, Array<uint8>& out) noexcept {
    const char* modeName = mode == render::RenderLaneMode::Thread ? "thread" : "inline";
    const bool fillChecks = checks.IsEmpty();
    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks cooked texture self-test";
    windowDesc.width = SceneCaptureWidth;
    windowDesc.height = SceneCaptureHeight;
    windowDesc.visible = false;
    auto window = platform::Window::Create(allocator, windowDesc);
    input::InputQueue queue(allocator);
    if (!window || !queue.TryInitialize(64)) {
        printf("  [NOT RUN] %s cooked textures (%s): window unavailable\n", name, modeName);
        return false;
    }

    // 설정 문맥(호스트와 같은 HostSettings — 모든 모듈의 설정, asset.cooked_root는 명령줄 --set과
    // 같은 길)이다.
    HostSettings host(allocator);
    HostSettingsOptions boot;
    boot.modules = &modules;
    boot.registerModuleTypes = &RegisterCollisionTypes;
    char engineDir[MaxPathBytes] = {};
    if (auto found = FindEngineDirectory(Span<char>(engineDir)); found) {
        boot.engineDir = StringView(engineDir, found.Value());
    }

    String assignment(allocator);
    const bool assigned = assignment.TryAppend("--set=asset.cooked_root=").IsOk() &&
                          assignment.TryAppend(cookedRoot).IsOk();
    const StringView arguments[] = {assignment.View()};
    const bool configured = assigned && host.Initialize(boot).IsOk() &&
                            host.ApplyCommandLine(Span<const StringView>(arguments)).IsOk() &&
                            host.CompleteStartup().IsOk();
    CookedCheck(configured, name, "the cooked root setting is accepted");
    if (!configured) {
        return false;
    }

    config::ConfigContext& settings = host.Context();
    const uint64 validationBefore = device.ValidationErrorCount();
    screen::ScreenHost screen(allocator, device, shaders::Host());
    CookedTextureHooks hooks(screen, *window.Value(), queue);
    auto started = screen.Start(*window.Value(), mode, hooks.Pump());
    CookedCheck(started.IsOk(), name, "render lane starts");
    if (!started) {
        return false;
    }

    UniquePtr<CookedTextureLibrary> library;
    uint32 requested = 0;
    {
        const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
        EngineConfig config;
        config.hooks = &hooks;
        config.modules = &modules;
        config.maxFrames = MaxCookedFrames;
        config.fixedFrameDeltaSeconds = FrameSeconds;
        config.settings = &settings;
        config.subsystems = Span<const EngineSubsystemType>(subsystems);
        config.registerTypes = &RegisterSandboxTypes;
        Engine engine(allocator);
        const bool initialized = engine.Initialize(config).IsOk();
        CookedCheck(initialized, name, "engine starts with the test content cook");
        if (initialized) {
            EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>();
            auto made = assets != nullptr
                            ? UniquePtr<CookedTextureLibrary>::TryMake(
                                  allocator, allocator, assets->Manager(), assets->Manifest(),
                                  engine.Types(), screen)
                            : Result<UniquePtr<CookedTextureLibrary>>::Failure(ErrorCode::NotFound);
            if (made) {
                library = Move(made.Value());
                auto all = library->RequestAll();
                requested = all ? all.Value() : 0;
                hooks.Attach(library.Get());
            }

            CookedCheck(requested != 0, name, "the cook lists texture assets");
            CookedCheck(engine.Run().IsOk(), name, "engine runs until every texture is drawn");
            CookedCheck(hooks.done, name, "every cooked texture settles before the frame limit");
            // OpenGL ES는 Windows 쿠킹의 대상 backend가 아니다: 장치가 못 하는 형식(BC 확장이
            // 없는 장치의 블록 압축 형식 등)은 실패가 아니라 NOT RUN 사유다.
            const bool cookTarget = device.Info().backend != graphics::Backend::OpenGL;
            for (uint32 request = 0; library && request < library->Requested(); ++request) {
                const ErrorCode error = library->FailureOf(request);
                if (error == ErrorCode::None) {
                    continue;
                }

                const StringView path = library->PathOf(request);
                if (!cookTarget && error == ErrorCode::UnsupportedCapability) {
                    if (fillChecks) {
                        printf("  [NOT RUN] %s cooked texture %.*s: the device cannot sample its "
                               "format (not a cook target backend)\n",
                               name, static_cast<int>(path.ByteCount()), path.Data());
                    }

                    continue;
                }

                CookedCheck(false, name,
                            "a cooked texture loads, converts and passes the upload check");
                printf("    %.*s: %s\n", static_cast<int>(path.ByteCount()), path.Data(),
                       ErrorCodeName(error));
            }

            CookedCheck(library != nullptr, name, "the cooked texture list is created");
            if (fillChecks && library) {
                for (uint32 request = 0; request < library->Requested(); ++request) {
                    const uint32 entry = library->EntryOf(request);
                    if (entry == screen::ScreenHost::NoInspectorEntry) {
                        continue;
                    }

                    ExpectationContext context;
                    context.texture = &screen.InspectorEntryAt(entry).texture;
                    context.request = request;
                    context.requested = library->Requested();
                    String path(allocator);
                    (void)path.TryAppend(library->PathOf(request));
                    context.path = path.CStr();
                    context.checks = &checks;
                    CookedCheck(
                        library->InspectCookedData(request, &ComputeExpectation, &context).IsOk(),
                        name, "cooked data is readable for the CPU reference");
                }

                cookedFormatsListed = true;
            }
        }

        CookedCheck(engine.Shutdown().IsOk(), name, "engine shutdown closes the render lane");
    }

    CookedCheck(screen.Lane().Failure() == ErrorCode::None &&
                    screen.Renderer().Stats().graphFailures == 0,
                name, "frames render without lane or graph failure");
    CookedCheck(screen.Lane().Handles().Count() == 0 && device.PendingRetireCount() == 0 &&
                    device.IsOwnerThread(),
                name, "lane drained with no remaining handles or device retires");
    CookedCheck(device.ValidationErrorCount() == validationBefore, name,
                "no validation errors from cooked textures");
    for (const SceneCheck& check : checks) {
        CheckCapturePixel(screen.Renderer().CapturedPixels(check.slot), check.x, check.y,
                          check.expected, SceneCaptureTolerance, name, modeName, check.what,
                          cookedFailures);
    }

    const usize slotBytes = static_cast<usize>(SceneCaptureWidth) * SceneCaptureHeight * 4;
    const uint32 frames = requested * FramesPerTexture;
    const uint32 slots = frames < screen::MaxCaptureSlots ? frames : screen::MaxCaptureSlots;
    out.Clear();
    if (out.TryResize(slotBytes * slots)) {
        for (uint32 slot = 0; slot < slots; ++slot) {
            const Array<uint8>& pixels = screen.Renderer().CapturedPixels(slot);
            for (usize index = 0; index < pixels.Size() && index < slotBytes; ++index) {
                out[slot * slotBytes + index] = pixels[index];
            }
        }
    }

    printf("  %s cooked textures (%s): %u requested, %u checks\n", name, modeName, requested,
           static_cast<uint32>(checks.Size()));
    return true;
}

} // namespace

uint32 RunCookedTextureChecks(const char* name, graphics::Device& device, Allocator& allocator,
                              ModuleLoader& modules) noexcept {
    cookedFailures = 0;
    if (const char* reason = WindowPresentUnavailableReason(device)) {
        printf("  [NOT RUN] %s cooked textures: %s\n", name, reason);
        return 0;
    }

    if (!screen::CanOpenEngineShaders(shaders::Host())) {
        printf("  [NOT RUN] %s cooked textures: no shader provider (Release)\n", name);
        return 0;
    }

    String root(allocator);
    if (!FindTestContentCook(root)) {
        printf("  [NOT RUN] %s cooked textures: no test content cook (run Commandlet asset-cook "
               "--profile=%.*s --project=Programs/Sandbox/TestContent)\n",
               name, static_cast<int>(ResolveRuntimeProfile(StringView()).ByteCount()),
               ResolveRuntimeProfile(StringView()).Data());
        return 0;
    }

    if (device.Info().backend == graphics::Backend::OpenGL) {
        // Windows 쿠킹 프로필의 graphicsBackends에 OpenGL이 없다 — 형식이 맞는 텍스처는 그대로
        // 대조하고, 못 올린 텍스처는 실패가 아니라 사유를 남긴다(아래 Failed 단언 대신).
        printf(
            "  %s cooked textures: OpenGL ES is not a graphics backend of the %.*s cook; textures "
            "are drawn where the device supports their formats\n",
            name, static_cast<int>(ResolveRuntimeProfile(StringView()).ByteCount()),
            ResolveRuntimeProfile(StringView()).Data());
    }

    Array<SceneCheck> checks(allocator);
    Array<uint8> thread(allocator);
    Array<uint8> inline_(allocator);
    const bool threadRan = RunMode(name, render::RenderLaneMode::Thread, device, allocator, modules,
                                   root.View(), checks, thread);
    const bool inlineRan = RunMode(name, render::RenderLaneMode::Inline, device, allocator, modules,
                                   root.View(), checks, inline_);
    if (threadRan && inlineRan) {
        bool same = thread.Size() == inline_.Size();
        for (usize index = 0; same && index < thread.Size(); ++index) {
            same = thread[index] == inline_[index];
        }

        CookedCheck(same, name, "thread and inline lane modes produce the same pixels");
    }

    if (threadRan && !thread.IsEmpty()) {
        const usize slotBytes = static_cast<usize>(SceneCaptureWidth) * SceneCaptureHeight * 4;
        CompareWithReferenceBackend("cooked textures", name, checks.View(), thread.View(),
                                    static_cast<uint32>(thread.Size() / slotBytes), cookedFailures);
    }

    printf("  [%s] %s cooked textures: the test content cook draws and matches the CPU "
           "reference\n",
           cookedFailures == 0 ? "PASS" : "FAIL", name);
    return cookedFailures;
}

} // namespace sandbox
