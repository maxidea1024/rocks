#include <Core/Module/ModuleEntry.h>

// SplashScreen 모듈의 진입점이다(Docs/Design/SplashScreen 3절). 시작할 일이 없다 — 호스트가
// SplashScreen::Show를 명시해서 부른다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, SplashScreen)
