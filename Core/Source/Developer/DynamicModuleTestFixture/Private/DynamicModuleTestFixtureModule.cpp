#include <Core/Module/ModuleEntry.h>
#include <DynamicModuleTestFixture/DynamicProbeModule.h>

namespace rocks::test {

Result<void> DynamicProbeModule::Startup(ModuleStartContext& context) noexcept {
    (void)context;
    started_ = true;
    return Result<void>::Success();
}

void DynamicProbeModule::Shutdown() noexcept {
    started_ = false;
}

int32 DynamicProbeModule::Answer() const noexcept {
    return ExpectedAnswer;
}

bool DynamicProbeModule::Started() const noexcept {
    return started_;
}

} // namespace rocks::test

// DynamicModuleTestFixture 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절).
ROCKS_IMPLEMENT_MODULE(rocks::test::DynamicProbeModule, DynamicModuleTestFixture)
