#include "../SplashPresenter.h"

// 스플래시 구현이 없는 플랫폼이다(Docs/Design/SplashScreen 15절 — macOS는 SP3, 모바일은 OS 실행
// 화면).
namespace rocks::splash {

Result<UniquePtr<SplashPresenter>> CreatePlatformSplashPresenter(Allocator& allocator) noexcept {
    (void)allocator;
    return Result<UniquePtr<SplashPresenter>>::Failure(ErrorCode::Unsupported);
}

void PostSplashDialogHide() noexcept {
}

} // namespace rocks::splash
