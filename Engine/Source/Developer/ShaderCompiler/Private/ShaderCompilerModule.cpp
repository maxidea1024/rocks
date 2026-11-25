#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/ModuleStartContext.h>
#include <ShaderCompiler/ModuleShaderCookRegistrySource.h>
#include <ShaderCompiler/ShaderLibraryCookStage.h>

namespace rocks::shader {

namespace {

constexpr LogCategory LogShaderCompilerModule{"ShaderCompiler"};

// ShaderCompiler 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.2절, ShaderSystem 16.1절 SSD44).
// Startup이 셰이더 라이브러리 비에셋 산출물 단계를 프로세스 단계 표(CookArtifactStages)에 명시
// 등록하고 Shutdown이 뺀다. 이미 다른 ShaderLibrary 단계가 있거나(시험의 가짜 단계) 표가 사용
// 중이면 Warning만 남기고 등록하지 않은 채 시작한다(Development 실행 파일·시험의 엔진 묶음도 이
// 모듈을 시작한다).
class ShaderCompilerModule final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override {
        auto source = UniquePtr<ModuleShaderCookRegistrySource>::TryMake(allocator_, allocator_,
                                                                         context.Loader());
        if (!source) {
            return Result<void>::Failure(source.Error());
        }

        ShaderLibraryCookStageConfig config;
        config.allocator = &allocator_;
        config.registry = source.Value().Get();
        auto stage = UniquePtr<ShaderLibraryCookStage>::TryMake(allocator_, config);
        if (!stage) {
            return Result<void>::Failure(stage.Error());
        }

        if (auto registered = CookArtifactStages().Register(*stage.Value()); !registered) {
            Log::Warning(LogShaderCompilerModule,
                         "The shader library cook stage is not registered ({Error}); another "
                         "shader library stage is registered or a cook is running",
                         ErrorCodeName(registered.Error()));
            return Result<void>::Success();
        }

        source_ = Move(source.Value());
        stage_ = Move(stage.Value());
        return Result<void>::Success();
    }

    void Shutdown() noexcept override {
        if (stage_) {
            (void)CookArtifactStages().Unregister(*stage_);
        }

        stage_.Reset();
        source_.Reset();
    }

private:
    // 단계·등록부 원천·관리자 저장소다(여러 스레드가 동시에 할당한다).
    SystemAllocator allocator_;
    UniquePtr<ModuleShaderCookRegistrySource> source_;
    UniquePtr<ShaderLibraryCookStage> stage_;
};

} // namespace

} // namespace rocks::shader

ROCKS_IMPLEMENT_MODULE(rocks::shader::ShaderCompilerModule, ShaderCompiler)
