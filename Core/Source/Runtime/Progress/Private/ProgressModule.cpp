#include <Core/Module/ModuleEntry.h>

// Progress 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다 — hub는 엔진·도구
// 시작 코드가 ProgressHub::Initialize로 켠다(Docs/Design/ProgressReporting 7.1절).
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Progress)
