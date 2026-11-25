#include <Core/Diagnostics/Log.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/ModuleStartContext.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <UiRender/UiShaderTypes.h>

namespace rocks::ui {

namespace {

constexpr LogCategory LogUiRender{"UiRender"};

// UiRender 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.12절). 호스트가 셰이더 타입 등록부를
// 서비스로 올렸으면 UI 셰이더 타입(UiShaderTypes — UiToolkit 9.1절 UW1a)을 등록한다. 등록 실패는
// 모듈 시작 실패다.
class UiRenderModule final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override {
        if (auto registered = shaders_.Startup(context); !registered) {
            Log::Error(LogUiRender, "UI shader types could not be registered: {Error}",
                       ErrorCodeName(registered.Error()));
            return registered;
        }

        return Result<void>::Success();
    }

    void Shutdown() noexcept override {
        shaders_.Shutdown();
    }

private:
    shader::ShaderTypeModuleRegistration shaders_{UiShaderTypes()};
};

} // namespace

} // namespace rocks::ui

ROCKS_IMPLEMENT_MODULE(rocks::ui::UiRenderModule, UiRender)
