#include <Core/Module/ModuleEntry.h>

// TextureTools 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다 — 임포터·hook은
// 호스트가 RegisterTextureImporters·RegisterTextureTypeHooks로 세션 표에 건다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, TextureTools)
