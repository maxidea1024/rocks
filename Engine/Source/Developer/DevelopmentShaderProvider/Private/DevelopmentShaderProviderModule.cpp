#include <DevelopmentShaderProvider/DevelopmentShaderProvider.h>

#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Move.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleStartContext.h>

namespace rocks::shader {

namespace {

constexpr LogCategory LogDevelopmentShaderProviderModule{"DevelopmentShaderProvider"};

// DevelopmentShaderProvider 모듈의 수명 객체다(Docs/Design/RenderBase 9.12절 4항,
// Docs/Design/ModuleLoader 3.12절). 호스트가 엔진 셰이더 호스트를 서비스로 올렸으면 공급자를 만들어
// 붙이고, Shutdown에서 떼고 워커를 멈춘다. 서비스가 없으면(셰이더를 쓰지 않는 호스트·도구) 아무
// 일도 하지 않는다. 호스트가 이미 다른 공급자를 붙였으면 그것을 존중한다(Info만 남긴다).
class DevelopmentShaderProviderModule final : public ModuleImplementation {
public:
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

        auto created = DevelopmentShaderProvider::TryCreate(allocator_, *host);
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
        return Result<void>::Success();
    }

    void Shutdown() noexcept override {
        if (host_ == nullptr) {
            return;
        }

        host_->DetachProvider(*provider_);
        provider_->Shutdown();
        provider_.Reset();
        host_ = nullptr;
    }

private:
    // 공급자·워커·너겟 캐시 저장소다(여러 스레드가 동시에 할당한다).
    SystemAllocator allocator_;
    render::EngineShaderHost* host_ = nullptr;
    UniquePtr<DevelopmentShaderProvider> provider_;
};

} // namespace

} // namespace rocks::shader

ROCKS_IMPLEMENT_MODULE(rocks::shader::DevelopmentShaderProviderModule, DevelopmentShaderProvider)
