#include <DevelopmentShaderProvider/DevelopmentShaderProvider.h>

#include <Config/CVar.h>
#include <Config/ConfigContext.h>
#include <Config/ConfigSnapshot.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Move.h>
#include <Engine/PendingWorkRegistry.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleStartContext.h>

namespace rocks::shader {

namespace {

constexpr LogCategory LogDevelopmentShaderProviderModule{"DevelopmentShaderProvider"};

// 감시 재컴파일·디버그 심볼 설정이다(Docs/Design/ShaderSystem 16.3절 SSD75). 켜고 끄면 감시
// 스레드가 생기거나 모든 셰이더 키가 바뀌므로 부팅 값이다.
constinit BootCVar<bool> RecompileOnSaveVar =
    cvar::Boot("shader.recompile_on_save", true)
        .Help("Watches the shader source folders and recompiles the shaders that depend on a "
              "saved file (development executables only). Fixed after startup.");

constinit BootCVar<bool> ShaderSymbolsVar =
    cvar::Boot("shader.symbols", false)
        .Help("Compiles shaders with debug symbols and writes the symbol files to "
              "Saved/ShaderSymbols/<format> (PDB for D3D, a SPIR-V module with debug lines for "
              "Vulkan). Fixed after startup.");

// DevelopmentShaderProvider 모듈의 수명 객체다(Docs/Design/RenderBase 9.12절 4항,
// Docs/Design/ModuleLoader 3.12절). 호스트가 엔진 셰이더 호스트를 서비스로 올렸으면 공급자를 만들어
// 붙이고, Shutdown에서 떼고 감시·워커를 멈춘다. 서비스가 없으면(셰이더를 쓰지 않는 호스트·도구)
// 아무 일도 하지 않는다. 호스트가 이미 다른 공급자를 붙였으면 그것을 존중한다(Info만 남긴다).
// 엔진 대기 작업 등록소 서비스가 있으면(Engine::Initialize의 엔진 묶음 — 진행 보고 8장 PG5)
// 공급자의 대기 작업 "Shaders"를 등록하고, Shutdown에서 그 서비스가 여전히 같은 등록소일 때만 뗀다.
class DevelopmentShaderProviderModule final : public ModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return context.Register(RecompileOnSaveVar, ShaderSymbolsVar);
    }

    Result<void> Startup(ModuleStartContext& context) noexcept override {
        render::EngineShaderHost* host = context.Services().Find<render::EngineShaderHost>();
        if (host == nullptr) {
            return Result<void>::Success();
        }

        if (host->Provider() != nullptr) {
            Log::Info(LogDevelopmentShaderProviderModule,
                      "A shader provider is already attached; the development shader provider "
                      "stays idle");
            return Result<void>::Success();
        }

        // CVar는 설정 문맥이 묶인 스레드에서만 읽는다. 설정 문맥 없이 엔진 묶음을 시작하는
        // 호스트(Sandbox self-test)는 기본값이다.
        DevelopmentShaderProviderOptions options;
        if (config::detail::CurrentBinding().snapshot != nullptr) {
            options.watchSources = RecompileOnSaveVar.Get();
            options.debugSymbols = ShaderSymbolsVar.Get();
        }
        auto created = DevelopmentShaderProvider::TryCreate(allocator_, *host, options);
        if (!created) {
            Log::Error(LogDevelopmentShaderProviderModule,
                       "The development shader provider could not be created: {Error}",
                       ErrorCodeName(created.Error()));
            return Result<void>::Failure(created.Error());
        }

        if (auto attached = host->AttachProvider(*created.Value()); !attached) {
            return attached;
        }

        provider_ = Move(created.Value());
        host_ = host;
        services_ = &context.Services();
        if (PendingWorkRegistry* registry = services_->Find<PendingWorkRegistry>()) {
            if (auto registered = registry->Register(provider_->PendingWork()); registered) {
                registry_ = registry;
            } else {
                Log::Warning(LogDevelopmentShaderProviderModule,
                             "Shader compiling is not reported as pending work: {Error}",
                             ErrorCodeName(registered.Error()));
            }
        }

        return Result<void>::Success();
    }

    void Shutdown() noexcept override {
        if (host_ == nullptr) {
            return;
        }

        // 등록을 먼저 뗀다(RemainingCount가 다른 스레드에서 불린다 — 공급자 Shutdown 전에).
        // 서비스가 사라졌거나 바뀌었으면 그 등록소는 이미 없다(같은 로더의 Engine 둘 — 진행 보고
        // 15장 11).
        if (registry_ != nullptr && services_->Find<PendingWorkRegistry>() == registry_) {
            (void)registry_->Unregister(provider_->PendingWork());
        }

        registry_ = nullptr;

        host_->DetachProvider(*provider_);
        provider_->Shutdown();
        provider_.Reset();
        host_ = nullptr;
    }

private:
    // 공급자·워커·너겟 캐시 저장소다(여러 스레드가 동시에 할당한다).
    SystemAllocator allocator_;
    render::EngineShaderHost* host_ = nullptr;
    // 대기 작업을 등록한 엔진 등록소와 그것을 찾은 서비스 표다(로더 수명).
    HostServices* services_ = nullptr;
    PendingWorkRegistry* registry_ = nullptr;
    UniquePtr<DevelopmentShaderProvider> provider_;
};

} // namespace

} // namespace rocks::shader

ROCKS_IMPLEMENT_MODULE(rocks::shader::DevelopmentShaderProviderModule, DevelopmentShaderProvider)
