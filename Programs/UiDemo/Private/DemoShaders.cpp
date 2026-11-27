#include "DemoShaders.h"

#include "DemoLog.h"

#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>

namespace rocks::ui {

namespace {

SystemAllocator& ProcessAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 엔진 셰이더 호스트다(프로세스 수명 — 공급자 모듈이 붙잡는다).
render::EngineShaderHost& Host() noexcept {
    static render::EngineShaderHost host(ProcessAllocator());
    return host;
}

} // namespace

DemoShaderHost::DemoShaderHost(ModuleLoader& modules) noexcept : modules_(modules) {
    if (auto attached = Host().Attach(modules.Services()); !attached) {
        Log::Error(LogUiDemo, "The engine shader host could not be attached: {Error}",
                   ErrorCodeName(attached.Error()));
        return;
    }

    attached_ = true;
    const ModuleLoadReport started = modules.StartEngineGroup();
    if (!started.IsOk()) {
        Log::Error(LogUiDemo,
                   "Engine modules could not start for the shader host: {Module} at {Stage}: "
                   "{Message}",
                   StringView(started.failed), StringView(started.stage),
                   StringView(started.message));
        return;
    }

    engineGroup_ = true;
    frozen_ = Host().Freeze().IsOk();
}

DemoShaderHost::~DemoShaderHost() noexcept {
    if (engineGroup_) {
        modules_.ShutdownEngineGroup();
    }

    if (attached_) {
        Host().Detach(modules_.Services());
    }
}

Result<void> OpenDemoShaders(render::EngineRenderResources& resources,
                             graphics::Device& device) noexcept {
    if (!resources.Open(device, Host()) || resources.Shaders() == nullptr) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
