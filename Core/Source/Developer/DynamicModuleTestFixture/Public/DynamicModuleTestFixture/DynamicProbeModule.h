#pragma once

#include <DynamicModuleTestApi/DynamicProbe.h>

namespace rocks::test {

// 실행 중 적재 시험 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.8절). 이 모듈을
// DynamicModules로 두는 시험 실행 파일은 이 헤더를 볼 수 없다(빌드가 include 경로에 넣지 않는다 —
// 시험이 __has_include로 확인한다). 시험은 DynamicProbe interface로만 쓴다.
class DynamicProbeModule final : public DynamicProbe {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override;
    void Shutdown() noexcept override;
    int32 Answer() const noexcept override;
    bool Started() const noexcept override;

private:
    bool started_ = false;
};

} // namespace rocks::test
