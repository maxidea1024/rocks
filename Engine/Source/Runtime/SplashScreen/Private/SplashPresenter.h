#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <SplashScreen/SplashImage.h>
#include <SplashScreen/SplashLayout.h>
#include <SplashScreen/SplashModel.h>

// 스플래시의 OS 창 부분이다(Docs/Design/SplashScreen 9.2절). 플랫폼마다 구현(Private/<Platform>/)이
// 있고, 시험은 SplashScreenTestAccess로 가짜 구현을 넣는다. Wake 말고는 스플래시 스레드에서만
// 부른다.
namespace rocks::splash {

// 창 생성 입력이다. 문자열은 Create 동안 유효하다.
struct SplashPresenterSetup {
    SplashKind kind = SplashKind::Game;
    StringView standardPath;
    StringView doublePath;
    // 창 제목(작업 표시줄·Alt+Tab에 보이는 이름 — Studio)이다.
    StringView windowTitle;
    // 배치 덮어쓰기다(검사를 통과한 값). Create 동안 유효하다.
    Span<const SplashTextPlacement> placements;
    const SplashBarPlacement* bar = nullptr;
};

// 실패한 OS 호출 이름의 최대 byte 수(NUL 포함)다.
inline constexpr usize MaxSplashCallNameBytes = 48;

// 창 생성 결과다. reason이 None이면 창이 떠 있다.
struct SplashCreateReport {
    SplashSkipReason reason = SplashSkipReason::None;
    // 원본 OS 오류(Windows HRESULT·GetLastError)다. 없으면 0이다.
    int64 osError = 0;
    // 실패한 OS 호출 이름(영어)이다.
    char call[MaxSplashCallNameBytes]{};
};

class SplashPresenter {
public:
    virtual ~SplashPresenter() = default;

    // 이미지를 풀고 창을 만들어 보인다(스플래시 스레드).
    virtual SplashCreateReport Create(const SplashPresenterSetup& setup) noexcept = 0;

    // 모델을 다시 그린다(스플래시 스레드).
    virtual void Present(const SplashModel& model) noexcept = 0;

    // 창을 잠시 숨기거나 다시 보인다(스플래시 스레드).
    virtual void SetSuspended(bool suspended) noexcept = 0;

    // OS 메시지를 처리하며 Wake나 제한 시간까지 기다린다(스플래시 스레드).
    virtual void Wait(uint64 timeoutNanoseconds) noexcept = 0;

    // Wait를 깨운다. 아무 스레드에서, Create 전에도 부를 수 있다(깨움은 다음 Wait에 남는다).
    virtual void Wake() noexcept = 0;

    // 창과 자원을 놓는다(스플래시 스레드, Create가 실패했어도 부른다).
    virtual void Destroy() noexcept = 0;
};

// presenter를 만드는 함수다. Show를 부른 스레드에서 불린다.
struct SplashPresenterFactory {
    Result<UniquePtr<SplashPresenter>> (*create)(Allocator& allocator,
                                                 void* context) noexcept = nullptr;
    void* context = nullptr;
};

// 이 플랫폼의 presenter다. 구현이 없는 플랫폼은 Unsupported다.
Result<UniquePtr<SplashPresenter>> CreatePlatformSplashPresenter(Allocator& allocator) noexcept;

// 시스템 대화상자 hook(9.5절)이 부른다: 떠 있는 스플래시 창을 숨기는 비동기 요청을 보낸다.
// 잠금·할당 없이 아무 스레드에서, 창이 없으면 아무 일도 하지 않는다.
void PostSplashDialogHide() noexcept;

// 칸 인덱스다.
constexpr usize SlotIndex(SplashTextSlot slot) noexcept {
    return static_cast<usize>(slot);
}

} // namespace rocks::splash
