#pragma once

#include <Base/Compiler.h>

// Android 액티비티 층이다(Docs/Design/AndroidPlatform.ko.md 3·4절). APK 프로그램(빌드 규칙
// AndroidPackage — define ROCKS_ANDROID_APK=1)에서 Launch가 ANativeActivity_onCreate를 정의하고
// 액티비티 콜백(UI 스레드)을 받는다. 엔진 본문은 Launch가 띄운 게임 스레드에서 돈다. 창·입력 큐
// 콜백은 등록된 listener(PlatformWindow Android backend)에 넘긴다. 선언은 모든 플랫폼에
// 보이지만(타입은 불투명 전방 선언) 정의는 Android Launch에만 있으므로 Android backend만 부른다.

struct ANativeActivity;
struct ANativeWindow;
struct AInputQueue;

namespace rocks::launch::android {

// 액티비티의 창·입력 큐·포커스 콜백을 받는 곳이다. 모든 함수는 UI 스레드에서 불리며, 게임 상태를
// 만지지 않고 자기 큐·gate만 다뤄야 한다.
class ActivityListener {
public:
    // 창이 생겼다. window는 OnWindowDestroyed까지 유효하다(필요하면 ANativeWindow_acquire).
    virtual void OnWindowCreated(ANativeWindow* window) noexcept = 0;
    // 창이 사라진다. 돌아오면 OS가 surface를 없애므로, 렌더가 그 surface를 놓을 때까지(제한 시간
    // 안에서) 기다린 뒤 돌아온다.
    virtual void OnWindowDestroyed(ANativeWindow* window) noexcept = 0;
    // 창 크기가 바뀌었다(회전 포함).
    virtual void OnWindowResized(ANativeWindow* window) noexcept = 0;
    // 창 포커스가 바뀌었다.
    virtual void OnFocusChanged(bool focused) noexcept = 0;
    // 입력 큐가 생겼다. 입력 스레드 looper에 붙인다(AInputQueue_attachLooper).
    virtual void OnInputQueueCreated(AInputQueue* queue) noexcept = 0;
    // 입력 큐가 사라진다. 돌아오기 전에 looper에서 뗀다(AInputQueue_detachLooper).
    virtual void OnInputQueueDestroyed(AInputQueue* queue) noexcept = 0;

protected:
    ~ActivityListener() = default;
};

// listener를 건다(nullptr이면 뗀다). 걸면 지금 있는 창·포커스·입력 큐를 곧바로 그 listener에
// 알린다(늦게 걸어도 놓치지 않게). 콜백과 같은 잠금 아래에서 바꾸므로, 돌아온 뒤에는 이전
// listener가 다시 불리지 않는다. 게임 스레드에서 부른다. APK가 아니면(액티비티가 없으면) 아무것도
// 하지 않는다.
//
// 사용 예(Android 창 backend):
//   launch::android::SetActivityListener(this);    // Window::Create
//   launch::android::SetActivityListener(nullptr); // ~Window — 그 전에 자기 gate를 연다
ROCKS_LAUNCH_API void SetActivityListener(ActivityListener* listener) noexcept;

// 지금 액티비티다(APK가 아니면 nullptr). 수명은 프로세스와 같다(onDestroy 뒤 프로세스가 끝난다).
ROCKS_LAUNCH_API ANativeActivity* CurrentActivity() noexcept;

} // namespace rocks::launch::android
