#include <Core/Module/ModuleEntry.h>

// Launch 모듈의 진입점이다(Docs/Design/ModuleLoader.ko.md 3.2절). 시작할 일이 없다. 모듈 진입점은
// 프로세스 진입점(Launch/EntryPoint.h의 ROCKS_IMPLEMENT_APPLICATION)과 다르다. 로더 통합(Launch가
// 로더를 만들고 부트 모듈을 시작하는 일)은 모듈 로더
// ML1이다(Docs/Design/CrashHandlingAndLaunch.ko.md 21절).
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Launch)
