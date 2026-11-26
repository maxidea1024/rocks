#pragma once

#include <Base/Types.h>

// 시스템 모달 대화상자 앞에서 부르는 hook이다(Docs/Design/SplashScreen 9.5절). 시작 화면(스플래시)
// 같은 창이 OS 대화상자(Fatal·assert 대화상자, 메시지 상자)를 가리지 않게, 대화상자를 띄우는 코드가
// 띄우기 직전에 NotifySystemDialogOpening을 부르고 hook을 건 쪽이 자기 창을 숨긴다. UE는 대화상자를
// 띄우는 곳마다 FPlatformSplash::Hide를 부른다 — Rocks는 이 한 곳으로 모은다.
//
// 사용 예:
//   // 창을 가진 쪽(스플래시 Show)
//   if (!platform::SetSystemDialogHook(&HideSplashForDialog)) { ... }   // 이미 다른 hook이 있다
//   ...
//   platform::ClearSystemDialogHook(&HideSplashForDialog);
//
//   // 대화상자를 띄우는 쪽
//   platform::NotifySystemDialogOpening();
//   MessageBoxW(...);
namespace rocks::platform {

// hook 함수다. 아무 스레드에서, 크래시 처리 경로에서도 불릴 수 있다. 잠금·할당·창 API의 동기 호출을
// 하지 않고 표시를 세우거나 비동기 메시지만 보낸다.
using SystemDialogHook = void (*)() noexcept;

// hook을 건다. 이미 다른 hook이 걸려 있으면 false이고 바꾸지 않는다(hook은 하나 — 지금 쓰는 곳은
// 스플래시뿐이다). 같은 hook을 다시 걸면 true다. null은 false다. 아무 스레드에서 부른다.
ROCKS_PLATFORMBASE_API bool SetSystemDialogHook(SystemDialogHook hook) noexcept;

// hook이 지금 걸린 hook과 같을 때만 뗀다(다른 쪽의 hook을 떼지 않는다). 아무 스레드에서 부른다.
// 떼는 동안 다른 스레드의 NotifySystemDialogOpening이 이미 읽은 hook은 그대로 불릴 수 있으므로,
// hook 함수는 자기 창이 사라진 뒤에도 안전해야 한다(표시만 세운다).
ROCKS_PLATFORMBASE_API void ClearSystemDialogHook(SystemDialogHook hook) noexcept;

// 시스템 모달 대화상자를 띄우기 직전에 부른다. hook이 있으면 부르고 없으면 아무 일도 하지 않는다.
// 아무 스레드, 크래시 경로에서도 부를 수 있다(잠금 없음).
ROCKS_PLATFORMBASE_API void NotifySystemDialogOpening() noexcept;

} // namespace rocks::platform
