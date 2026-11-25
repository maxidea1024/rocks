#include <Core/Module/ModuleEntry.h>
#include <Core/Module/ModuleImplementation.h>

namespace rocks::text {

namespace {

// TextLayoutBackend 모듈 진입점이다(Docs/Design/Font 3장). backend·face의 vtable과 HarfBuzz·ICU
// 할당 hook이 이 모듈 코드에 있고 라이브러리 정적 객체가 프로세스 끝까지 그 hook을 부르므로 내리지
// 않는다.
class TextLayoutBackendModule final : public ModuleImplementation {
public:
    bool SupportsUnload() const noexcept override {
        return false;
    }
};

} // namespace

} // namespace rocks::text

ROCKS_IMPLEMENT_MODULE(rocks::text::TextLayoutBackendModule, TextLayoutBackend)
