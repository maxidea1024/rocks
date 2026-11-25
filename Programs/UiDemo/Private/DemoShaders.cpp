#include "DemoShaders.h"

#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/EngineShaderTypes.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <UiRender/UiShaderTypes.h>

#if UIDEMO_SHADER_PROVIDER
#include <DevelopmentShaderProvider/DevelopmentShaderProvider.h>
#endif

namespace rocks::ui {

namespace {

inline constexpr LogCategory LogUiDemo{"UiDemo"};

SystemAllocator& ProcessAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 등록부다(처음 쓸 때 등록·Freeze한다 — 프로세스 수명).
shader::ShaderTypeRegistry* Registry() noexcept {
    static shader::ShaderTypeRegistry registry(ProcessAllocator());
    static const bool ready = registry.Register(render::EngineShaderTypes()) &&
                              registry.Register(UiShaderTypes()) && registry.Freeze();
    if (!ready) {
        Log::Error(LogUiDemo, "UI demo shader types failed to register: {Type}: {Reason}",
                   registry.LastFailure().typeName, registry.LastFailure().reason);
        return nullptr;
    }

    return &registry;
}

#if UIDEMO_SHADER_PROVIDER
shader::DevelopmentShaderProvider* Provider() noexcept {
    static shader::DevelopmentShaderProvider provider(
        {.allocator = &ProcessAllocator(), .mounts = {}, .hostTarget = "uidemo"});
    return &provider;
}
#endif

render::ShaderSetDesc SetDesc(Allocator& allocator, graphics::Device& device) noexcept {
    render::ShaderSetDesc desc;
    desc.allocator = &allocator;
    desc.device = &device;
    desc.registry = Registry();
    desc.library = shader::ShaderLibraryKind::Engine;
#if UIDEMO_SHADER_PROVIDER
    desc.formatId = shader::DevelopmentShaderFormat(device.Info().backend);
#endif
    return desc;
}

render::ShaderSet* active = nullptr;

} // namespace

DemoShaderSet::DemoShaderSet(Allocator& allocator, graphics::Device& device) noexcept
    : set_(SetDesc(allocator, device)), previous_(active) {
#if UIDEMO_SHADER_PROVIDER
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
        ready_ = true;
    } else {
        Log::Error(LogUiDemo, "UI demo shaders are not available: {Problem}", set_.ProblemDetail());
    }
}

DemoShaderSet::~DemoShaderSet() noexcept {
    if (ready_) {
        active = previous_;
    }

    set_.Retire();
}

StringView DemoShaderSet::Problem() const noexcept {
    return Registry() == nullptr ? StringView("UI demo shader types failed to register")
                                 : set_.ProblemDetail();
}

render::ShaderSet& ActiveShaders() noexcept {
    ROCKS_ASSERT(active != nullptr);
    return *active;
}

void ShutdownDemoShaders() noexcept {
#if UIDEMO_SHADER_PROVIDER
    Provider()->Shutdown();
#endif
}

} // namespace rocks::ui
