#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Graphics/GraphicsLog.h>
#include <RenderBase/EngineShaderTypes.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <stdio.h>
#include <string.h>

#if SANDBOX_SHADER_PROVIDER
#include <DevelopmentShaderProvider/DevelopmentShaderProvider.h>
#endif

// Sandbox 셰이더 집합과 Development 공급자(Docs/Design/ShaderSystem 16.1절 SSD19, 12.3.1절
// SSD16)다.
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
};
#undef SANDBOX_VERTEX
#undef SANDBOX_PIXEL
#undef SANDBOX_COMPUTE
#undef SANDBOX_VERTEX_IF
#undef SANDBOX_PIXEL_IF
#undef SANDBOX_PIXEL_DEV_IF

SystemAllocator& ProcessAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 등록부다(처음 쓸 때 등록·Freeze한다 — 프로세스 수명).
shader::ShaderTypeRegistry* Registry() noexcept {
    static shader::ShaderTypeRegistry registry(ProcessAllocator());
    // 엔진 셰이더 타입(RenderBase 11.4절 2항)도 함께 등록한다(엔진 호스트에서는 RenderBase 모듈이
    // 호스트 등록부에 넣는다).
    static bool ready = registry.Register(Types) &&
                        registry.Register(render::EngineShaderTypes()) && registry.Freeze();
    if (!ready) {
        Log::Error(graphics::LogGraphics,
                   "Sandbox shader types failed to register: {Type}: {Reason}",
                   registry.LastFailure().typeName, registry.LastFailure().reason);
        return nullptr;
    }

    return &registry;
}

// 장치 backend의 셰이더 형식 id다(호스트 Windows TargetProfile 대신 — SH2d 전 고정 표).
StringView FormatFor(graphics::Backend backend) noexcept {
    switch (backend) {
    case graphics::Backend::D3D12:
        return "D3D12_SM6";
    case graphics::Backend::D3D11:
        return "D3D11_SM5";
    case graphics::Backend::Vulkan:
        return "VULKAN_SM5";
    case graphics::Backend::OpenGL:
        return "GLES31_PC";
    case graphics::Backend::Metal:
        return "METAL_SM5";
    }

    return StringView();
}

render::ShaderSet* active = nullptr;

#if SANDBOX_SHADER_PROVIDER

// Sandbox 원문(Programs/Sandbox/Shaders)을 /Plugin/Sandbox로 mount한다(SSD19).
constexpr shader::ShaderSourceMountDesc SandboxMounts[] = {
    {"/Plugin/Sandbox", "Programs/Sandbox/Shaders"}};

// Development 공급자다(Developer DevelopmentShaderProvider — 프로세스 수명).
shader::DevelopmentShaderProvider* Provider() noexcept {
    static shader::DevelopmentShaderProvider provider(
        {.allocator = &ProcessAllocator(), .mounts = SandboxMounts, .hostTarget = "sandbox"});
    return &provider;
}

#endif

render::ShaderSetDesc SetDesc(Allocator& allocator, graphics::Device& device) noexcept {
    render::ShaderSetDesc desc;
    desc.allocator = &allocator;
    desc.device = &device;
    desc.registry = Registry();
    desc.library = shader::ShaderLibraryKind::Engine;
    desc.formatId = FormatFor(device.Info().backend);
    return desc;
}

} // namespace

Span<const ShaderTypeDesc* const> AllTypes() noexcept {
    return Types;
}

DeviceShaders::DeviceShaders(Allocator& allocator, graphics::Device& device) noexcept
    : set_(SetDesc(allocator, device)) {
#if SANDBOX_SHADER_PROVIDER
    render::ShaderProvider* provider = Provider();
#else
    render::ShaderProvider* provider = nullptr;
#endif
    if (Registry() == nullptr) {
        return;
    }

    if (auto opened = set_.OpenFromProvider(provider); !opened) {
        return;
    }

    if (set_.WaitUntilSettled() == render::ShaderSetState::Ready) {
        active = &set_;
        active_ = true;
    }
}

DeviceShaders::~DeviceShaders() noexcept {
    if (active_) {
        active = nullptr;
    }

    set_.Retire();
}

bool DeviceShaders::IsReady() const noexcept {
    return active_;
}

StringView DeviceShaders::Problem() const noexcept {
    return Registry() == nullptr ? StringView("Sandbox shader types failed to register")
                                 : set_.ProblemDetail();
}

bool Has(const ShaderTypeDesc& type) noexcept {
    return active != nullptr && active->FindEntry(type.nameHash).IsOk();
}

render::ShaderSet* ActiveSet() noexcept {
    return active;
}

const shader::ShaderBindingLayout* Layout(const ShaderTypeDesc& type) noexcept {
    if (active == nullptr) {
        return nullptr;
    }

    auto entry = active->FindEntry(type.nameHash);
    return entry ? active->BindingLayout(entry.Value()) : nullptr;
}

Result<graphics::ShaderHandle> Get(const ShaderTypeDesc& type) noexcept {
    if (active == nullptr) {
        return Result<graphics::ShaderHandle>::Failure(ErrorCode::NotReady);
    }

    auto entry = active->FindEntry(type.nameHash);
    if (!entry) {
        return Result<graphics::ShaderHandle>::Failure(entry.Error());
    }

    return active->DeviceShader(entry.Value());
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

void ShutdownProvider() noexcept {
#if SANDBOX_SHADER_PROVIDER
    Provider()->Shutdown();
#endif
}

} // namespace sandbox::shaders
