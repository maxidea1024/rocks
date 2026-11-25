#include <Core/Module/ModuleEntry.h>

// MaterialCompiler 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다 — 번역은
// 호출자(쿠커·에디터)가 함수로 부른다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, MaterialCompiler)
