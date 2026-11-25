#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Graphics/GraphicsLog.h>
#include <HostConfig/HostSettings.h>
#include <ModuleLoader/HostServices.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <stdio.h>
#include <string.h>

// Sandbox 셰이더 연결(Docs/Design/ShaderSystem 16.1절 SSD19, Docs/Design/RenderBase 9.12절)이다.
namespace sandbox::shaders {

using namespace rocks;

namespace {

#define SANDBOX_VERTEX(Variable, File, Entry) &Variable,
#define SANDBOX_PIXEL(Variable, File, Entry) &Variable,
#define SANDBOX_COMPUTE(Variable, File, Entry) &Variable,
#define SANDBOX_VERTEX_IF(Variable, File, Entry, Filter) &Variable,
#define SANDBOX_PIXEL_IF(Variable, File, Entry, Filter) &Variable,
#define SANDBOX_PIXEL_DEV_IF(Variable, File, Entry, Filter) &Variable,
const ShaderTypeDesc* const Types[] = {
#include "SandboxShaderTypes.inl"
    &MathFloatProbe, &MathUintProbe, &ColorFloatProbe, &ShadingFloatProbe};
#undef SANDBOX_VERTEX
#undef SANDBOX_PIXEL
#undef SANDBOX_COMPUTE
#undef SANDBOX_VERTEX_IF
#undef SANDBOX_PIXEL_IF
#undef SANDBOX_PIXEL_DEV_IF

// 경로 버퍼 byte 수다.
constexpr usize PathBytes = 1024;

SystemAllocator& ProcessAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 활성 자원이다(DeviceShaders가 연 동안).
render::EngineRenderResources* active = nullptr;
bool hostPrepared = false;

// self-test면 Sandbox 타입과 /Plugin/Sandbox 원문 디렉터리(엔진 폴더의 형제
// Programs/Sandbox/Shaders)를 한 번 넣는다. 엔진 실행은 아무것도 넣지 않는다(SSD48).
Result<void> PrepareHost(render::EngineShaderHost& host, SandboxShaderScope scope) noexcept {
    if (hostPrepared || scope == SandboxShaderScope::Engine) {
        return Result<void>::Success();
    }

    if (auto registered = host.Registry().Register(Types); !registered) {
        Log::Error(graphics::LogGraphics,
                   "Sandbox shader types failed to register: {Type}: {Reason}",
                   StringView(host.Registry().LastFailure().typeName),
                   StringView(host.Registry().LastFailure().reason));
        return registered;
    }

    char engine[PathBytes] = {};
    auto found = FindEngineDirectory(Span<char>(engine, sizeof(engine)));
    if (!found) {
        Log::Error(
            graphics::LogGraphics,
            "The engine folder was not found; Sandbox shader sources are not mounted ({Error})",
            ErrorCodeName(found.Error()));
        return Result<void>::Failure(found.Error());
    }

    char path[PathBytes];
    snprintf(path, sizeof(path), "%s/../Programs/Sandbox/Shaders", engine);
    if (auto added = host.AddSourceDirectory("/Plugin/Sandbox", StringView(path)); !added) {
        Log::Error(graphics::LogGraphics, "Sandbox shader sources could not be added: {Error}",
                   ErrorCodeName(added.Error()));
        return added;
    }

    // 엔진 셰이더 시험 루트(Docs/Design/EngineShaders 7.6절 — 쿠킹하지 않는 시험 전용 플러그인
    // 루트).
    snprintf(path, sizeof(path), "%s/../Programs/Sandbox/EngineShaderTests", engine);
    if (auto added = host.AddSourceDirectory("/Plugin/EngineShaderTests", StringView(path));
        !added) {
        Log::Error(graphics::LogGraphics, "Engine shader test sources could not be added: {Error}",
                   ErrorCodeName(added.Error()));
        return added;
    }

    hostPrepared = true;
    return Result<void>::Success();
}

} // namespace

Span<const ShaderTypeDesc* const> AllTypes() noexcept {
    return Types;
}

render::EngineShaderHost& Host() noexcept {
    static render::EngineShaderHost host(ProcessAllocator());
    return host;
}

Result<void> AttachHost(ModuleLoader& modules, SandboxShaderScope scope) noexcept {
    render::EngineShaderHost& host = Host();
    if (auto prepared = PrepareHost(host, scope); !prepared) {
        return prepared;
    }

    if (auto attached = host.Attach(modules.Services()); !attached) {
        Log::Error(graphics::LogGraphics, "The engine shader host could not be attached: {Error}",
                   ErrorCodeName(attached.Error()));
        return attached;
    }

    return Result<void>::Success();
}

void DetachHost(ModuleLoader& modules) noexcept {
    Host().Detach(modules.Services());
}

SelfTestShaderHost::SelfTestShaderHost(ModuleLoader& modules) noexcept : modules_(modules) {
    attached_ = AttachHost(modules, SandboxShaderScope::SelfTest).IsOk();
    if (!attached_) {
        return;
    }

    const ModuleLoadReport started = modules.StartEngineGroup();
    if (!started.IsOk()) {
        Log::Error(graphics::LogGraphics,
                   "Engine modules could not start for the shader host: {Module} at {Stage}: "
                   "{Message}",
                   StringView(started.failed), StringView(started.stage),
                   StringView(started.message));
        return;
    }

    engineGroup_ = true;
    frozen_ = Host().Freeze().IsOk();
}

SelfTestShaderHost::~SelfTestShaderHost() noexcept {
    if (engineGroup_) {
        modules_.ShutdownEngineGroup();
    }

    if (attached_) {
        DetachHost(modules_);
    }
}

DeviceShaders::DeviceShaders(Allocator& allocator, graphics::Device& device) noexcept
    : device_(device), resources_(allocator) {
    if (resources_.Open(device, Host()) && active == nullptr) {
        active = &resources_;
        active_ = true;
    }
}

DeviceShaders::~DeviceShaders() noexcept {
    if (active_) {
        active = nullptr;
    }

    resources_.Retire(device_);
}

bool DeviceShaders::IsReady() const noexcept {
    return active_;
}

StringView DeviceShaders::Problem() const noexcept {
    return resources_.Problem();
}

bool Has(const ShaderTypeDesc& type) noexcept {
    render::ShaderSet* set = ActiveSet();
    return set != nullptr && set->FindEntry(type.nameHash).IsOk();
}

render::ShaderSet* ActiveSet() noexcept {
    return active != nullptr ? active->Shaders() : nullptr;
}

const render::BuiltinResources* ActiveBuiltins() noexcept {
    return active != nullptr ? &active->Builtins() : nullptr;
}

const shader::ShaderBindingLayout* Layout(const ShaderTypeDesc& type) noexcept {
    render::ShaderSet* set = ActiveSet();
    if (set == nullptr) {
        return nullptr;
    }

    auto entry = set->FindEntry(type.nameHash);
    return entry ? set->BindingLayout(entry.Value()) : nullptr;
}

Result<graphics::ShaderHandle> Get(const ShaderTypeDesc& type) noexcept {
    render::ShaderSet* set = ActiveSet();
    if (set == nullptr) {
        return Result<graphics::ShaderHandle>::Failure(ErrorCode::NotReady);
    }

    auto entry = set->FindEntry(type.nameHash);
    if (!entry) {
        return Result<graphics::ShaderHandle>::Failure(entry.Error());
    }

    return set->DeviceShader(entry.Value());
}

Result<graphics::PipelineHandle> CreateGraphicsPipeline(graphics::Device& device,
                                                        const ShaderTypeDesc& vertex,
                                                        const ShaderTypeDesc& pixel,
                                                        graphics::PipelineDesc desc) noexcept {
    auto vs = Get(vertex);
    if (!vs) {
        return Result<graphics::PipelineHandle>::Failure(vs.Error());
    }

    auto ps = Get(pixel);
    if (!ps) {
        return Result<graphics::PipelineHandle>::Failure(ps.Error());
    }

    desc.vertexShader = vs.Value();
    desc.pixelShader = ps.Value();
    return device.CreatePipeline(desc);
}

Result<graphics::PipelineHandle>
CreateComputePipeline(graphics::Device& device, const ShaderTypeDesc& compute,
                      graphics::ComputePipelineDesc desc) noexcept {
    auto cs = Get(compute);
    if (!cs) {
        return Result<graphics::PipelineHandle>::Failure(cs.Error());
    }

    desc.computeShader = cs.Value();
    return device.CreateComputePipeline(desc);
}

} // namespace sandbox::shaders
