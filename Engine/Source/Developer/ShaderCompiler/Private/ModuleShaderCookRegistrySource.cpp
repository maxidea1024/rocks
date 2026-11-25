#include <ShaderCompiler/ModuleShaderCookRegistrySource.h>

#include <Core/Move.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleLoader.h>
#include <stdio.h>

// 제품 쿠커의 셰이더 타입 등록부 원천(Docs/Design/BuildTargets 3.10절 4항)이다. 규칙은 공개 헤더
// 주석을 따른다.
namespace rocks::shader {

ModuleShaderCookRegistrySource::ModuleShaderCookRegistrySource(Allocator& allocator,
                                                               ModuleLoader& loader) noexcept
    : allocator_(allocator), loader_(loader) {
}

ModuleShaderCookRegistrySource::~ModuleShaderCookRegistrySource() noexcept {
    Release();
}

Result<const ShaderTypeRegistry*> ModuleShaderCookRegistrySource::Acquire() noexcept {
    using Out = Result<const ShaderTypeRegistry*>;
    problem_[0] = '\0';
    if (acquired_ != nullptr) {
        return Out::Failure(ErrorCode::Busy);
    }

    HostServices& services = loader_.Services();
    if (const ShaderTypeRegistry* existing = services.Find<ShaderTypeRegistry>()) {
        if (!existing->IsFrozen()) {
            snprintf(problem_, sizeof(problem_), "the host shader type registry is not frozen yet");
            return Out::Failure(ErrorCode::NotReady);
        }

        acquired_ = existing;
        return Out::Success(existing);
    }

    auto created = UniquePtr<ShaderTypeRegistry>::TryMake(allocator_, allocator_);
    if (!created) {
        return Out::Failure(created.Error());
    }

    if (auto registered = services.Register(*created.Value()); !registered) {
        snprintf(problem_, sizeof(problem_), "the shader type registry service: %s",
                 ErrorCodeName(registered.Error()));
        return Out::Failure(registered.Error());
    }

    registry_ = Move(created.Value());
    const auto failModules = [this](const ModuleLoadReport& report) noexcept {
        snprintf(problem_, sizeof(problem_), "module %s failed at %s: %s", report.failed,
                 report.stage, report.message);
    };
    const ModuleLoadReport engine = loader_.StartEngineGroup();
    if (!engine.IsOk()) {
        failModules(engine);
        Release();
        return Out::Failure(ErrorCode::DependencyFailed);
    }

    engineStarted_ = true;
    const ModuleLoadReport post = loader_.StartPhase(LoadingPhase::PostEngineInit);
    if (!post.IsOk()) {
        failModules(post);
        Release();
        return Out::Failure(ErrorCode::DependencyFailed);
    }

    if (auto frozen = registry_->Freeze(); !frozen) {
        const ShaderTypeRegistrationFailure& failure = registry_->LastFailure();
        snprintf(problem_, sizeof(problem_), "shader types could not be frozen: %s: %s",
                 failure.typeName, failure.reason);
        Release();
        return Out::Failure(frozen.Error());
    }

    acquired_ = registry_.Get();
    return Out::Success(acquired_);
}

void ModuleShaderCookRegistrySource::Release() noexcept {
    acquired_ = nullptr;
    if (engineStarted_) {
        loader_.ShutdownEngineGroup();
        engineStarted_ = false;
    }

    if (registry_) {
        (void)loader_.Services().Unregister(*registry_);
        registry_.Reset();
    }
}

StringView ModuleShaderCookRegistrySource::Problem() const noexcept {
    return StringView(problem_);
}

} // namespace rocks::shader
