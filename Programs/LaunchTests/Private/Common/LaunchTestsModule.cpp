#include <Core/Module/ModuleEntry.h>

// Launch 시험 실행 파일 모듈의 진입점이다(Docs/Design/ModuleLoader.ko.md 3.2절). 이 소스는
// LaunchProbe·LaunchWindowedProbe·LaunchEmbeddedHost가 함께 컴파일하므로 이름 자리에 빌드가 넣은
// ROCKS_MODULE_NAME을 쓴다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, ROCKS_MODULE_NAME)
