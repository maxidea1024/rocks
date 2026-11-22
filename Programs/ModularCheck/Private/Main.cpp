#include <Launch/EntryPoint.h>
#include <ModuleLoader/ModuleLoader.h>

#include <stdio.h>

// Modular 링크 확인 프로그램이다(Docs/Design/BuildTargets.ko.md 3.8절 BS8d2). Modular 빌드에서 이
// 본문까지 왔으면 진입 스텁 → Launch Modular 진입 → manifest 로더(모든 모듈 공유 라이브러리 적재) →
// 호스트 앱 기술 → 부트 모듈 시작이 끝난 것이다(Docs/Design/ModuleLoader.ko.md 3.14절). 로더 표의
// 모듈 수와 부트 모듈 시작 여부를 찍고, 부트 모듈이 시작해 있으면 0이다.
namespace {

int RunMain(rocks::launch::ApplicationContext& context) noexcept {
    rocks::ModuleLoader& modules = context.Modules();
    const bool booted = modules.IsStarted("Core") && modules.IsStarted("ModuleLoader") &&
                        modules.IsStarted("Launch");
    printf("ModularCheck: %llu modules, boot modules %s\n",
           static_cast<unsigned long long>(modules.ModuleCount()),
           booted ? "started" : "not started");
    return booted ? 0 : 1;
}

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description{
        .name = "ModularCheck", .kind = rocks::launch::ApplicationKind::Tool, .run = &RunMain};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
