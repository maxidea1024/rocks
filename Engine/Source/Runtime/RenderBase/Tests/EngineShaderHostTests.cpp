#include "RecordingDevice.h"
#include "RegistryShaderSet.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Move.h>
#include <Engine/Engine.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleLoader.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/RenderLane.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>

// 엔진 호스트 셰이더 연결 시험이다(Docs/Design/RenderBase 9.12절 RR11): 호스트 서비스(등록부·원문
// 디렉터리·공급자·쿠킹 원천 자리), 엔진 묶음 모듈의 등록 → Freeze → 렌더 lane 시작의 엔진 셰이더
// 집합·내장 자원 팩·준비 완료, 등록 누락·Freeze 전·공급자 없음·공급 실패의 진단, 쿠킹 원천 우선과
// NotFound 넘김, Freeze 뒤 엔진 묶음 재시작 실패. 공급자는 등록부의 모든 조합을 가짜 코드로 담은
// 라이브러리를 이 호출 안에서 넘긴다(RegistryShaderSet — 시간에 기대지 않는다).
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::render::testing::RegistryShaderSet;

namespace engine_shader_host_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 요청마다 등록부의 라이브러리를 만들어 넘기는 공급자다. fail이면 공급 실패를 알린다.
class FakeProvider final : public ShaderProvider {
public:
    Result<void> RequestLibrary(const ShaderProvisionRequest& request,
                                ShaderSet& set) noexcept override {
        ++requests;
        lastFormat = request.formatId;
        Array<uint8> bytes(Allocator_());
        if (fail || !RegistryShaderSet::BuildLibrary(Allocator_(), *request.registry, bytes)) {
            set.CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed),
                                  Array<uint8>(Allocator_()), "fake compile failed");
            return Result<void>::Success();
        }

        set.CompleteProvision(Result<void>::Success(), Move(bytes), StringView());
        return Result<void>::Success();
    }

    uint32 requests = 0;
    StringView lastFormat;
    bool fail = false;
};

// 쿠킹 라이브러리 원천이다. 쿠킹 backend는 D3D12 하나(cookedBackend가 거짓이면 Vulkan — 장치
// backend가 쿠킹에 없음), 형식은 D3D12_SM6이다. 형식이 있으면 등록부의 라이브러리, 없으면 NotFound,
// error면 그 오류다.
class FakeCookedSource final : public CookedShaderLibrarySource {
public:
    explicit FakeCookedSource(const shader::ShaderTypeRegistry& registry) noexcept
        : registry_(registry) {
    }

    Span<const graphics::Backend> Backends() const noexcept override {
        return cookedBackend ? Span<const graphics::Backend>(&d3d12_, 1)
                             : Span<const graphics::Backend>(&vulkan_, 1);
    }

    Span<const StringView> Formats(graphics::Backend backend) const noexcept override {
        return backend == graphics::Backend::D3D12 && cookedBackend
                   ? Span<const StringView>(&format_, 1)
                   : Span<const StringView>();
    }

    Result<void> ReadLibrary(StringView formatId, shader::ShaderLibraryKind library,
                             Array<uint8>& out) noexcept override {
        ++reads;
        if (error != ErrorCode::None) {
            return Result<void>::Failure(error);
        }

        if (!hasFormat || formatId != StringView("D3D12_SM6") ||
            library != shader::ShaderLibraryKind::Engine) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        return RegistryShaderSet::BuildLibrary(Allocator_(), registry_, out)
                   ? Result<void>::Success()
                   : Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    StringView Problem() const noexcept override {
        return error != ErrorCode::None ? StringView("fake read failure") : StringView();
    }

    uint32 reads = 0;
    bool hasFormat = true;
    bool cookedBackend = true;
    ErrorCode error = ErrorCode::None;

private:
    const shader::ShaderTypeRegistry& registry_;
    const graphics::Backend d3d12_ = graphics::Backend::D3D12;
    const graphics::Backend vulkan_ = graphics::Backend::Vulkan;
    const StringView format_ = "D3D12_SM6";
};

// 렌더 lane 시작 단계에서 엔진 렌더 자원을 여는 처리기다(9.12절 2항).
class ResourcesProcessor final : public RenderLaneProcessor {
public:
    explicit ResourcesProcessor(const EngineShaderHost& host) noexcept
        : resources(Allocator_()), host_(host) {
    }

    Result<void> OnLaneStart(RenderLaneContext& context) noexcept override {
        return resources.Open(context.Device(), host_);
    }

    void OnLaneStop(RenderLaneContext& context) noexcept override {
        resources.Retire(context.Device());
    }

    EngineRenderResources resources;

private:
    const EngineShaderHost& host_;
};

RenderLaneConfig InlineConfig(RenderLaneProcessor& processor) noexcept {
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Inline;
    config.processor = &processor;
    config.hangTimeoutMs = 0;
    return config;
}

bool RegisterEngine(shader::ShaderTypeRegistry& registry) noexcept {
    return registry.Register(EngineShaderTypes()) &&
           registry.RegisterVertexInputTypes(EngineVertexInputTypes());
}

// 엔진 시작 경로: 호스트가 등록부를 서비스로 올리면 Engine::Initialize가 시작한 엔진 묶음에서
// RenderBase 모듈이 엔진 셰이더 타입을 넣고, Freeze 뒤 렌더 lane 시작이 집합·팩·준비 완료를
// 만든다. 엔진 종료(엔진 묶음 종료)는 등록을 거두고, 떼면 서비스가 사라진다.
TEST(RenderBase_EngineShaderHostOpensAtLaneStart) {
    ModuleLoader& modules = test::ProcessModules();
    REQUIRE(modules.EngineGroupUsers() == 0);
    EngineShaderHost host(Allocator_());
    CHECK(host.AddSourceDirectory("/Plugin/HostTests", "C:/HostTests/Shaders"));
    CHECK(host.AddSourceDirectory("/Plugin/hosttests", "C:/Other").Error() ==
          ErrorCode::AlreadyExists);
    CHECK(host.AddSourceDirectory("/Engine", "C:/Engine").Error() == ErrorCode::InvalidArgument);
    CHECK(host.AddSourceDirectory("Plugin", "C:/Plugin").Error() == ErrorCode::InvalidArgument);
    CHECK(host.AddSourceDirectory("/Plugin/Empty", "").Error() == ErrorCode::InvalidArgument);
    REQUIRE(host.SourceDirectories().Size() == 1);
    CHECK(host.SourceDirectories()[0].virtualRoot == StringView("/Plugin/HostTests"));
    CHECK(host.SourceDirectories()[0].directory == StringView("C:/HostTests/Shaders"));

    REQUIRE(host.Attach(modules.Services()));
    CHECK(host.Attach(modules.Services()).Error() == ErrorCode::Busy);
    CHECK(modules.Services().Find<EngineShaderHost>() == &host);
    CHECK(modules.Services().Find<shader::ShaderTypeRegistry>() == &host.Registry());
    FakeProvider provider;
    FakeProvider other;
    REQUIRE(host.AttachProvider(provider));
    CHECK(host.AttachProvider(other).Error() == ErrorCode::Busy);
    CHECK(host.Provider() == &provider);
    {
        EngineConfig config;
        config.modules = &modules;
        Engine engine(Allocator_());
        REQUIRE(engine.Initialize(config));
        CHECK(host.Registry().Find(FullscreenTriangleVertexType.hash) != nullptr);
        CHECK(host.Registry().Find(BuiltinMeshVertexType.hash) != nullptr);
        CHECK(host.Registry().FindVertexInputType(BuiltinVertexInputName.hash) != nullptr);
        CHECK(!host.IsFrozen());

        RecordingDevice device(Allocator_());
        {
            // Freeze 전에는 열지 않는다(로더 스레드의 Freeze 자리를 알려 준다).
            EngineRenderResources early(Allocator_());
            CHECK(early.Open(device, host).Error() == ErrorCode::NotReady);
            CHECK(early.Problem().Contains(StringView("not frozen")));
            CHECK(provider.requests == 0);
        }

        REQUIRE(host.Freeze());
        CHECK(host.Freeze());
        CHECK(host.AddSourceDirectory("/Plugin/Late", "C:/Late").Error() == ErrorCode::ScopeClosed);
        CHECK(host.Registry().Register(EngineShaderTypes()).Error() == ErrorCode::ScopeClosed);

        ResourcesProcessor processor(host);
        RenderLane lane(Allocator_(), device);
        REQUIRE(lane.Start(InlineConfig(processor)));
        auto ticket = lane.BeginFrame();
        REQUIRE(ticket);
        CHECK(lane.IsReady());
        CHECK(processor.resources.IsReady() && processor.resources.Builtins().IsReady());
        CHECK(!processor.resources.OpenedFromCookedLibrary());
        CHECK(provider.requests == 1 && provider.lastFormat == StringView("D3D12_SM6"));
        ShaderSet* set = processor.resources.Shaders();
        REQUIRE(set != nullptr);
        CHECK(set->State() == ShaderSetState::Ready);
        CHECK(set->FindEntry(CopyTexturePixelType.hash).IsOk());
        CHECK(processor.resources.Builtins().ShaderEntries().meshVertex != NoShaderSetEntry);
        CHECK(processor.resources.Problem().IsEmpty());
        lane.AbandonFrame(Move(ticket.Value()));
        CHECK(lane.Shutdown());
        CHECK(!processor.resources.IsReady() && !processor.resources.Builtins().IsReady());
        CHECK(processor.resources.Shaders() == nullptr);
        CHECK(device.PendingRetireCount() == 0);
        CHECK(engine.Shutdown());
    }

    // 엔진 묶음 종료가 RenderBase 등록을 거뒀다(Freeze 뒤에도 거둘 수 있다).
    CHECK(host.Registry().Types().Size() == 0);
    CHECK(host.Registry().VertexInputTypes().Size() == 0);
    host.DetachProvider(other);
    CHECK(host.Provider() == &provider);
    host.DetachProvider(provider);
    CHECK(host.Provider() == nullptr);
    host.Detach(modules.Services());
    CHECK(!host.IsAttached());
    CHECK(modules.Services().Find<EngineShaderHost>() == nullptr);
    CHECK(modules.Services().Find<shader::ShaderTypeRegistry>() == nullptr);
}

// 등록 누락·공급 경로의 진단: fallback 메시 셰이더 타입이 없는 등록부는 집합이 열려도 팩의 fallback
// 셰이더가 없어 NotFound(빠진 이름)이고 준비 완료가 없다. 공급자가 없으면 UnsupportedCapability
// (NoProvider), 공급이 실패하면 DependencyFailed(공급자 진단)다.
TEST(RenderBase_EngineRenderResourcesDiagnoseMissingPieces) {
    RecordingDevice device(Allocator_());
    {
        // 메시 셰이더를 등록하는 모듈이 시작하지 않은 경우다(전체화면 두 타입만 있다).
        EngineShaderHost host(Allocator_());
        const shader::ShaderTypeDesc* const fullscreen[] = {EngineShaderTypes()[0],
                                                            EngineShaderTypes()[1]};
        REQUIRE(host.Registry().Register(fullscreen));
        REQUIRE(host.Freeze());
        FakeProvider provider;
        REQUIRE(host.AttachProvider(provider));
        EngineRenderResources resources(Allocator_());
        CHECK(resources.Open(device, host).Error() == ErrorCode::NotFound);
        CHECK(resources.Problem().Contains(StringView("builtin resources")));
        CHECK(resources.Problem().Contains(StringView("BuiltinMeshVertex")));
        CHECK(!resources.IsReady() && resources.Shaders() == nullptr);

        // 렌더 lane 시작 단계에서도 같은 오류로 준비 완료가 없다.
        ResourcesProcessor processor(host);
        RenderLane lane(Allocator_(), device);
        REQUIRE(lane.Start(InlineConfig(processor)));
        CHECK(lane.BeginFrame().Error() == ErrorCode::NotFound);
        CHECK(!lane.IsReady() && !processor.resources.IsReady());
        (void)lane.Shutdown();
        host.DetachProvider(provider);
    }

    EngineShaderHost host(Allocator_());
    REQUIRE(RegisterEngine(host.Registry()));
    REQUIRE(host.Freeze());
    {
        EngineRenderResources resources(Allocator_());
        CHECK(resources.Open(device, host).Error() == ErrorCode::UnsupportedCapability);
        CHECK(resources.Problem().Contains(StringView("NoProvider")));
    }

    FakeProvider provider;
    provider.fail = true;
    REQUIRE(host.AttachProvider(provider));
    {
        EngineRenderResources resources(Allocator_());
        CHECK(resources.Open(device, host).Error() == ErrorCode::DependencyFailed);
        CHECK(resources.Problem().Contains(StringView("ProviderFailed")));
        CHECK(resources.Problem().Contains(StringView("fake compile failed")));

        // 실패한 집합은 다음 Open이 부수고 다시 연다.
        provider.fail = false;
        REQUIRE(resources.Open(device, host));
        CHECK(resources.IsReady() && resources.Open(device, host).Error() == ErrorCode::Busy);
        resources.Retire(device);
        CHECK(!resources.IsReady());
    }

    host.DetachProvider(provider);
    CHECK(device.WaitIdle());
    CHECK(device.PendingRetireCount() == 0);
}

// 쿠킹 원천이 있으면 그 라이브러리가 먼저다. 원천에 형식이 없으면(NotFound) 공급자로 넘어가고, 다른
// 읽기 오류는 그 오류다. 자리는 하나다(Busy).
TEST(RenderBase_EngineRenderResourcesPreferCookedLibraries) {
    RecordingDevice device(Allocator_());
    EngineShaderHost host(Allocator_());
    REQUIRE(RegisterEngine(host.Registry()));
    REQUIRE(host.Freeze());
    FakeCookedSource cooked(host.Registry());
    FakeCookedSource second(host.Registry());
    FakeProvider provider;
    REQUIRE(host.AttachCookedLibraries(cooked));
    CHECK(host.AttachCookedLibraries(second).Error() == ErrorCode::Busy);
    REQUIRE(host.AttachProvider(provider));
    {
        EngineRenderResources resources(Allocator_());
        REQUIRE(resources.Open(device, host));
        CHECK(resources.OpenedFromCookedLibrary() && cooked.reads == 1 && provider.requests == 0);
        CHECK(resources.Builtins().IsReady());
        resources.Retire(device);
    }

    // 장치 backend(D3D12)가 쿠킹에 없으면 공급자 경로다(ShaderSystem SSD16) — 원천을 읽지 않는다.
    cooked.cookedBackend = false;
    {
        EngineRenderResources resources(Allocator_());
        REQUIRE(resources.Open(device, host));
        CHECK(!resources.OpenedFromCookedLibrary() && cooked.reads == 1 && provider.requests == 1);
        CHECK(resources.FormatId() == StringView("D3D12_SM6"));
        resources.Retire(device);
    }

    // backend는 쿠킹에 있는데 그 형식의 라이브러리가 없으면 공급자로 넘기지 않고 Unsupported다.
    cooked.cookedBackend = true;
    cooked.hasFormat = false;
    {
        EngineRenderResources resources(Allocator_());
        CHECK(resources.Open(device, host).Error() == ErrorCode::Unsupported);
        CHECK(resources.Problem().Contains(StringView("no library of this kind")));
        CHECK(cooked.reads == 2 && provider.requests == 1);
    }

    cooked.hasFormat = true;
    cooked.error = ErrorCode::Cancelled;
    {
        EngineRenderResources resources(Allocator_());
        CHECK(resources.Open(device, host).Error() == ErrorCode::Cancelled);
        CHECK(resources.Problem().Contains(StringView("cooked shader library")));
        CHECK(resources.Problem().Contains(StringView("fake read failure")));
        CHECK(provider.requests == 1);
    }

    host.DetachCookedLibraries(second);
    CHECK(host.CookedLibraries() == &cooked);
    host.DetachCookedLibraries(cooked);
    CHECK(host.CookedLibraries() == nullptr);
    host.DetachProvider(provider);
    CHECK(device.WaitIdle());
    CHECK(device.PendingRetireCount() == 0);
}

// 재컴파일하는 공급자다(ShaderSystem 16.3절 SSD76): revision을 시험이 올리고, fail이면 공급 실패다.
class ReloadingProvider final : public ShaderProvider {
public:
    Result<void> RequestLibrary(const ShaderProvisionRequest& request,
                                ShaderSet& set) noexcept override {
        ++requests;
        Array<uint8> bytes(Allocator_());
        if (fail || !RegistryShaderSet::BuildLibrary(Allocator_(), *request.registry, bytes)) {
            set.CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed),
                                  Array<uint8>(Allocator_()), "fake reload failed");
            return Result<void>::Success();
        }

        set.CompleteProvision(Result<void>::Success(), Move(bytes), StringView());
        return Result<void>::Success();
    }

    uint64 LibraryRevision(StringView formatId,
                           shader::ShaderLibraryKind library) const noexcept override {
        return formatId == StringView("D3D12_SM6") && library == shader::ShaderLibraryKind::Engine
                   ? revision
                   : 0;
    }

    uint32 requests = 0;
    uint64 revision = 1;
    bool fail = false;
};

// 감시 재컴파일의 교체 연결(9.8절 "SH4 연결"): revision이 오르면 프레임 경계에서 새 집합으로 바꾸고
// 옛 집합의 cache 항목을 회수하며, 진행 중 프레임의 lease가 풀릴 때까지 옛 집합을 남긴다. 공급이
// 실패한 revision은 거부하고 현재를 유지하며 다시 열지 않는다. 쿠킹 라이브러리 집합은 다시 열지
// 않는다.
TEST(RenderBase_EngineRenderResourcesSwapProviderRevisions) {
    RecordingDevice device(Allocator_());
    EngineShaderHost host(Allocator_());
    REQUIRE(RegisterEngine(host.Registry()));
    REQUIRE(host.Freeze());
    ReloadingProvider provider;
    REQUIRE(host.AttachProvider(provider));
    {
        EngineRenderResources resources(Allocator_());
        PipelineCache cache(Allocator_(), device);
        REQUIRE(resources.Open(device, host));
        CHECK(resources.ShaderRevision() == 1 && provider.requests == 1);
        // revision이 그대로면 아무 일도 하지 않는다.
        CHECK(!resources.ApplyShaderUpdates(&cache) && provider.requests == 1);

        ShaderSet* first = resources.Shaders();
        REQUIRE(first != nullptr);
        auto copy = first->FindEntry(CopyTexturePixelType.hash);
        REQUIRE(copy);
        auto request =
            FullscreenPipelineRequest(*first, copy.Value(), graphics::PixelFormat::RGBA8Unorm);
        REQUIRE(request);
        REQUIRE(cache.FindOrCreate(request.Value()));
        CHECK(cache.Size() == 1);
        // 진행 중 프레임이 옛 집합을 잡고 있다.
        REQUIRE(first->TryAcquireLease());

        provider.revision = 2;
        CHECK(resources.ApplyShaderUpdates(&cache));
        ShaderSet* second = resources.Shaders();
        REQUIRE(second != nullptr);
        CHECK(second != first && second->Epoch() != first->Epoch());
        CHECK(second->State() == ShaderSetState::Ready);
        CHECK(first->State() == ShaderSetState::Retired && !first->TryAcquireLease());
        CHECK(cache.Size() == 0);
        CHECK(resources.ShaderRevision() == 2 && resources.ShaderSwaps() == 1 &&
              provider.requests == 2);
        // 내장 자원 팩은 새 집합의 항목을 가리킨다.
        const BuiltinShaderEntries& entries = resources.Builtins().ShaderEntries();
        CHECK(second->Entry(entries.copyPixel) != nullptr &&
              entries.copyPixel == second->FindEntry(CopyTexturePixelType.hash).Value());
        first->ReleaseLease();
        CHECK(!resources.ApplyShaderUpdates(&cache));

        // 공급 실패 revision은 거부하고 현재를 유지한다(다시 열지 않는다).
        provider.fail = true;
        provider.revision = 3;
        CHECK(!resources.ApplyShaderUpdates(&cache));
        CHECK(resources.Shaders() == second && resources.ShaderRevision() == 2 &&
              resources.ShaderRejections() == 1 && provider.requests == 3);
        CHECK(!resources.ApplyShaderUpdates(&cache) && provider.requests == 3);

        // 고친 revision은 다시 교체한다.
        provider.fail = false;
        provider.revision = 4;
        CHECK(resources.ApplyShaderUpdates(&cache));
        CHECK(resources.Shaders() != second && resources.ShaderRevision() == 4 &&
              resources.ShaderSwaps() == 2);
        cache.Clear();
        resources.Retire(device);
        CHECK(!resources.IsReady() && resources.Shaders() == nullptr);
        CHECK(!resources.ApplyShaderUpdates(&cache));
    }

    // 쿠킹 라이브러리로 연 집합은 revision이 올라도 다시 열지 않는다.
    FakeCookedSource cooked(host.Registry());
    REQUIRE(host.AttachCookedLibraries(cooked));
    {
        EngineRenderResources resources(Allocator_());
        REQUIRE(resources.Open(device, host));
        REQUIRE(resources.OpenedFromCookedLibrary());
        const uint32 requests = provider.requests;
        provider.revision = 9;
        CHECK(!resources.ApplyShaderUpdates(nullptr) && provider.requests == requests);
        CHECK(resources.ShaderRevision() == 0);
        resources.Retire(device);
    }

    host.DetachCookedLibraries(cooked);
    host.DetachProvider(provider);
    CHECK(device.WaitIdle());
    CHECK(device.PendingRetireCount() == 0);
}

// 정적 모듈 표로 새 로더를 만드는 시험이라 Monolithic에서만 돈다(Docs/Design/ModuleLoader 3.14절).
#if ROCKS_MONOLITHIC
// 늦은 등록(9.12절 1항): Freeze한 등록부로 엔진 묶음을 다시 시작하면 RenderBase 등록이
// ScopeClosed라 그 모듈 시작이 실패하고 엔진 묶음이 되돌아간다. 다른 형식이 이미 서비스에 있으면
// Attach는 아무것도 올리지 않는다.
TEST(RenderBase_EngineShaderHostRejectsLateRegistration) {
    auto created = ModuleLoader::TryCreate(Allocator_(), GeneratedModuleTable());
    REQUIRE(created);
    UniquePtr<ModuleLoader> modules = Move(created.Value());
    {
        shader::ShaderTypeRegistry foreign(Allocator_());
        REQUIRE(modules->Services().Register(foreign));
        EngineShaderHost host(Allocator_());
        CHECK(host.Attach(modules->Services()).Error() == ErrorCode::AlreadyRegistered);
        CHECK(!host.IsAttached() && modules->Services().Find<EngineShaderHost>() == nullptr);
        CHECK(modules->Services().Unregister(foreign));
    }

    EngineShaderHost host(Allocator_());
    REQUIRE(host.Attach(modules->Services()));
    REQUIRE(modules->StartEngineGroup().IsOk());
    CHECK(modules->IsStarted("RenderBase"));
    CHECK(host.Registry().Find(FullscreenTriangleVertexType.hash) != nullptr);
    REQUIRE(host.Freeze());
    modules->ShutdownEngineGroup();
    CHECK(host.Registry().Types().Size() == 0);

    const ModuleLoadReport restarted = modules->StartEngineGroup();
    CHECK(!restarted.IsOk());
    CHECK(StringView(restarted.failed) == StringView("RenderBase"));
    CHECK(modules->EngineGroupUsers() == 0 && !modules->IsStarted("RenderBase"));
    CHECK(host.Registry().Types().Size() == 0);
    modules->ShutdownGroup(ModuleGroup::Process);
    host.Detach(modules->Services());
}
#endif // ROCKS_MONOLITHIC

} // namespace engine_shader_host_tests_file
