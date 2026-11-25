#include <Core/Diagnostics/Log.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleStartContext.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/GpuTrace.h>
#include <ShaderCore/ShaderTypeRegistry.h>

namespace rocks::render {

namespace {

constexpr LogCategory LogRenderBase{"RenderBase"};

// RenderBase 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.12절).
// - 호스트가 셰이더 타입 등록부를 서비스로 올렸으면 엔진 셰이더 타입(EngineShaderTypes,
//   Docs/Design/RenderBase 11.4절 2항)을 등록한다. 등록 실패는 모듈 시작 실패다.
// - 호스트가 실행 중인 trace 세션을 서비스로 등록했으면 GPU trace 채널 "gpu"와 기록 형을 등록하고
//   Shutdown에서 푼다. 등록한 동안 모듈 lease를 잡는다(채널·형 객체가 이 모듈의 데이터다). GPU
//   계측은 진단 기능이라 등록 실패는 Warning이고 시작은 성공한다.
class RenderBaseModule final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override {
        if (auto registered = shaders_.Startup(context); !registered) {
            Log::Error(LogRenderBase, "engine shader types could not be registered: {Error}",
                       ErrorCodeName(registered.Error()));
            return registered;
        }

        TraceSession* session = context.Services().Find<TraceSession>();
        if (session == nullptr || !session->IsRunning()) {
            return Result<void>::Success();
        }

        if (auto registered = RegisterGpuTraceTypes(*session); !registered) {
            Log::Warning(LogRenderBase, "GPU trace types could not be registered: {Error}",
                         ErrorCodeName(registered.Error()));
            return Result<void>::Success();
        }

        session_ = session;
        lease_ = context.Lease();
        return Result<void>::Success();
    }

    void Shutdown() noexcept override {
        shaders_.Shutdown();
        if (session_ == nullptr) {
            return;
        }

        // 멈춘 세션은 Stop이 등록을 이미 거뒀다. 다른 형이 GPU 채널을 써서 다 풀지 못하면(Busy)
        // Warning만 남긴다(한계 — Docs/Design/ModuleLoader 3.12절).
        if (session_->IsRunning()) {
            if (auto unregistered = UnregisterGpuTraceTypes(*session_); !unregistered) {
                Log::Warning(LogRenderBase, "GPU trace types could not be unregistered: {Error}",
                             ErrorCodeName(unregistered.Error()));
            }
        }

        session_ = nullptr;
        lease_.Reset();
    }

private:
    // 엔진 셰이더 타입 등록이다(등록부가 없는 호스트에서는 아무 일도 하지 않는다).
    shader::ShaderTypeModuleRegistration shaders_{EngineShaderTypes()};
    // GPU trace 형을 등록한 세션이다. 호스트가 이 모듈의 묶음을 내린 뒤에 서비스를 뗀다.
    TraceSession* session_ = nullptr;
    ModuleLease lease_;
};

} // namespace

} // namespace rocks::render

ROCKS_IMPLEMENT_MODULE(rocks::render::RenderBaseModule, RenderBase)
