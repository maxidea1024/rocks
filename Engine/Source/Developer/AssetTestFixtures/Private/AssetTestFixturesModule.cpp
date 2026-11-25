#include <Core/Module/ModuleEntry.h>

// AssetTestFixtures 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이
// 없다(시험이 타입을 자기 registry에 등록한다).
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, AssetTestFixtures)
