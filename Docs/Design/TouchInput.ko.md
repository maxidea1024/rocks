# 터치 입력 — 멀티터치·제스처·터치 조작·모바일 backend

개정 2 (2026-11-24, 개정 1에 UE 5.8 교차 검증 반영). 설계만 한다. 구현은 아래 6절 순서를 따르며 지금 착수하지 않는다.

## 1. 목적과 범위

엔진 1차 목표의 입력(마우스·키보드·게임패드)은 Sandbox까지 연결됐다(EngineRuntime 21.5절). 터치는 아직 코드도 설계도 없다.

- 전체 계획은 touch를 요구한다(EngineImplementationPlan 20.4절 "key/gamepad/touch").
- 입력 상세 계획 19.3절은 "터치·펜·tablet은 I2+ 확장 후보"라고만 적었다.
- mobile은 Android를 먼저 한다는 결정(PlatformLayer "모바일 플랫폼")이 있는데, Android 창·입력 backend가 없다. `PlatformWindow`는 Android에서 Unsupported를 돌려준다.

이 문서가 그 빈자리를 채운다.

- 이 문서는 입력 상세 계획 19.3절의 터치 문장과 "가짜 값을 생성하지 않는다" 규칙의 터치 압력 부분을 대체한다(압력은 지원 표지와 함께 둔다 — 3.1절). 그 문서는 500줄을 넘어 장별 분할 대상이고, 맡은 세션이 다음 수정 때 이 문서를 가리키게 고친다.
- 범위:
  - 멀티터치 이벤트와 상태, 손가락 번호 배정
  - OS backend(Windows 터치 화면·펜, Android)
  - 제스처 인식, 액션 매핑의 터치 원천
  - 터치 조작(가상 조이스틱), UI 터치 라우팅, Actor 터치 이벤트
- 범위 밖:
  - iOS(보류 결정)
  - 펜 기울기·지우개·hover
  - OS 수준 제스처 이벤트(TD12)
  - 접근성 터치 탐색

## 2. UE 5.8 기준

UE 5.8 소스(`C:\github\UnrealEngine`, 경로는 `Engine/Source` 기준)에서 확인했다.

| 층 | UE 5.8 | 내용 |
|---|---|---|
| OS → 앱 | `ApplicationCore/Public/GenericPlatform/GenericApplicationMessageHandler.h:378–414` | `OnTouchStarted(Window, Location, Force, TouchIndex, PlatformUserId, DeviceId)`·`OnTouchMoved`·`OnTouchEnded`·`OnTouchForceChanged`·`OnTouchFirstMove` |
| OS 제스처 | 같은 파일 `:157`, `:354` | `EGestureEvent`(Scroll·Magnify·Swipe·Rotate·LongPress·Pan·Tap·Flick)와 `OnTouchGesture`. 트랙패드나 OS 인식기가 보낸다 |
| 손가락 번호 | `InputCore/Classes/InputCoreTypes.h:254–274, 754` | `ETouchIndex::Touch1–Touch10`과 `CursorPointerIndex`, `EKeys::NUM_TOUCH_KEYS = 11`. `CursorPointerIndex`는 실제 마우스 커서의 포인터 번호다(`Slate/.../SlateApplicationBase.cpp:19`). 손가락과 겹치지 않게 따로 둔다 |
| Windows | `ApplicationCore/Private/Windows/WindowsApplication.cpp:73–85, 2386–2418, 3700, 3760` | `WM_POINTER*`가 기본이다(`UsePointerMessagesForTouch`·`UsePointerMessagesForPen` 모두 true). 터치(`PT_TOUCH`)와 펜(`PT_PEN`)을 모두 터치로 보내고, 압력은 항상 1.0이다. `WM_TOUCH`는 대체 경로다. `WM_POINTERCAPTURECHANGED`는 처리하지 않는다 |
| Android 진입 | `Programs/UnrealBuildTool/Platform/Android/UEDeployAndroid.cs:6057`, `Launch/Private/Android/LaunchAndroid.cpp:20` | UE 자체 Java 클래스 `GameActivity`(`NativeActivity` 상속)와 `android_native_app_glue`를 쓴다. AndroidX games-activity가 아니다 |
| Android 입력 | `LaunchAndroid.cpp:1193–1352`, `ApplicationCore/Private/Android/AndroidInputInterface.cpp:1200–1280` | `AInputEvent`·`AMotionEvent_*`를 읽는다. DOWN·POINTER_DOWN은 Began, MOVE는 Moved, UP·POINTER_UP·CANCEL·OUTSIDE는 Ended가 된다. `pointerId`를 그대로 TouchIndex로 쓰고, 압력은 1.0 고정이다 |
| Android 뒤로 가기 | `LaunchAndroid.cpp:232, 1436` | 기본적으로 Back 키를 소비하고 키 이벤트(`EKeys::Android_Back`)만 보낸다(`GAllowJavaBackButtonEvent = false`). 앱을 끝내지 않는다 |
| 게임 상태 | `Engine/Classes/GameFramework/PlayerInput.h:417–448, 900` | `UE::FTouchState{DeviceId, ViewportRelativeLocation, Pressure}`를 `NUM_TOUCH_KEYS`개 고정 배열로 둔다(`GetTouchStates`). 범위 밖 번호는 버린다(`PlayerInput.cpp:520`) |
| 제스처 인식 | `Engine/Private/GestureRecognizer.cpp`, `PlayerInput.cpp:2393–2399`, `Config/BaseInput.ini:15` | `bEnableGestureRecognizer`(기본 false)가 켜져 있을 때만 돈다. 결과는 `Gesture_Pinch`·`Gesture_Rotate`(`TouchStates[0]`·`[1]` 두 칸)와 `Gesture_Flick`(한 손가락, 0.25 s 안에 떼고 이동 거리 제곱이 10000 초과)이다. Pan·Tap·LongPress 키는 있지만 인식기가 만들지 않는다 |
| Pinch 세부 | `GestureRecognizer.cpp:37–65` | 두 손가락이 닿으면 바로 시작한다. 값은 거리 제곱 비율(`NewDistanceSq / AnchorDistanceSq`, 시작 1.0)이다. 10000은 두 점이 직전 pinch 점에서 100 px 넘게 벗어나면 기준 거리를 다시 잡는 임계다 |
| 액션 매핑 | `EKeys::Touch1–10`, `EKeys::Gesture_*` | 터치와 제스처를 키처럼 매핑에 쓴다(Enhanced Input도 같은 키) |
| Actor 터치 | `GameFramework/PlayerController.h:535–543`, `PlayerController.cpp:219` | `bEnableTouchEvents`(기본 true)·`bEnableTouchOverEvents`. 손가락 아래 Actor·Component에 터치 시작·끝·들어감·나감을 보낸다 |
| 터치 조작 | `GameFramework/TouchInterface.h`, `Slate/Private/Widgets/Input/SVirtualJoystick.cpp:75–96` | `UTouchInterface` 에셋은 컨트롤 목록과 공통 값으로 이루어진다. 공통 값은 활성·비활성 불투명도, 비활성까지 시간, 재중심 시간, 활성 지연, 시작 지연, 재중심 금지다. Slate `SVirtualJoystick`이 그린다. 위치·크기 값은 음수면 오른쪽·아래 가장자리 기준, 1 이하면 화면 비율, 1 초과면 DPI 배율을 곱한 픽셀이다 |
| 마우스 → 터치 흉내 | `UInputSettings::bUseMouseForTouch`(기본 false), `SlateApplication.cpp:5258–5265, 5994–5998` | 왼쪽 버튼을 손가락 0번(Touch1) 터치로 바꾸고, 그 버튼의 마우스 이벤트는 보내지 않는다 |
| 터치 → 마우스 fallback | `SlateApplication.cpp:905–929, 5369` | UI가 처리하지 않은 터치를 마우스로 다시 보낸다(`bTouchFallbackToMouse`, 기본 true) |

## 3. 이벤트와 상태 (Input 모듈)

### 3.1 이벤트

`input::InputEventType`에 셋을 더한다.

| 이벤트 | 필드 | 뜻 |
|---|---|---|
| `TouchBegan` | `touchIndex`·`x`·`y`·`force`·`hasForce` | 손가락(또는 펜)이 닿았다 |
| `TouchMoved` | `touchIndex`·`x`·`y`·`force`·`hasForce` | 위치나 압력이 바뀌었다. UE `OnTouchMoved`와 `OnTouchForceChanged`를 하나로 합쳤다(TD3) |
| `TouchEnded` | `touchIndex`·`x`·`y`·`cancelled` | 떨어졌다. OS가 취소한 경우 `cancelled = true`다. 취소 원인은 Android `ACTION_CANCEL`, Windows `POINTER_FLAG_CANCELED`·capture 상실, 포커스 상실, 큐 손실 복구다 |

- **좌표:** 마우스와 같은 창 client 영역의 물리 픽셀, 좌상단 원점이다. 논리 단위 변환은 입력 상세 계획 19.3절의 좌표 타입 규칙(PW2 DPI 배율)을 따른다.
  - Android 렌더 surface가 창과 크기가 다르면(해상도 배율), backend는 창 좌표를 그대로 넘긴다. viewport 좌표 변환은 view 쪽이 한다(UE `ScreenRect`와 같은 자리).
- **압력:** `force`는 0–1로 정규화한 값이다. OS가 압력을 주면 그 값을 쓰고 `hasForce = true`로 둔다. 주지 않으면 `force = 1.0`, `hasForce = false`다.
  - UE는 Windows·Android 모두 1.0 고정이다. Rocks는 값이 있으면 넘긴다(TD13).
  - 게임이 압력을 모르는 장치와 진짜 1.0을 가를 수 있게 지원 표지를 둔다.
- **창:** 다중 창(PW1·PW5)이 오기 전까지는 창 하나다. PW5가 이벤트에 창 표지를 더할 때 터치도 같은 표지를 쓴다. 터치는 시작한 창에 묶이며, 끝날 때까지 그 창이 받는다(OS 포인터 capture와 같다).
- **손가락 번호:** `touchIndex`는 OS 포인터 ID가 아니라 backend가 배정한 번호다(3.2절).

### 3.2 손가락 번호

- 번호는 0부터 `MaxTouches - 1`(= 9)까지 10개다(UE `Touch1–Touch10`). 마우스 커서의 포인터 번호는 `MousePointerIndex`(= 10, UE `CursorPointerIndex`)로 따로 둔다. UI 포인터 이벤트(5.4절)가 마우스와 손가락을 한 번호 공간에서 구분할 때 쓴다. 두 값 모두 이름 있는 상수다.
- 배정기는 backend(PlatformWindow) 소유다. OS 포인터 ID를 받으면 비어 있는 가장 작은 번호를 준다.
  - 끝난 번호는 그 프레임 동안 다시 주지 않는다. 배정기는 프레임 경계(`InputState::BeginFrame` 뒤 호스트가 부르는 `Window::EndInputFrame`)에서 번호를 되돌린다.
  - 그래서 한 프레임 안에서 같은 번호가 끝났다가 다시 시작하는 일이 없고, 시작 위치도 덮어쓰지 않는다.
- Windows 포인터 ID는 작은 수가 아니라서 표가 필요하다. Android `pointerId`는 이미 작은 수이지만 같은 배정기를 써서 플랫폼마다 규칙을 하나로 둔다(TD2).
- 빈 번호가 없을 때(11번째 손가락) 이벤트를 만들지 않고, 그 포인터가 끝날 때까지 무시한다. UE도 범위 밖 번호를 버린다. 버린 수는 배정기가 `Window::DroppedTouchCount()`로 센다.

### 3.3 프레임 상태

`InputState`가 손가락마다 다음 값을 들고 `BeginFrame`에서 갱신한다. edge 규칙은 지금의 마우스 버튼 규칙(InputState.cpp)과 같다.

| 질의 | 뜻 |
|---|---|
| `IsTouchDown(index)` | 지금 닿아 있다 |
| `WasTouchBegan(index)`·`WasTouchEnded(index)` | 이번 프레임에 시작했다 / 끝났다. 한 프레임에 시작과 끝이 모두 오면 둘 다 true이고, 상태는 끝난 것이다 |
| `WasTouchCancelled(index)` | 이번 프레임의 끝이 취소였다 |
| `TouchPosition(index)`·`TouchStartPosition(index)`·`TouchDelta(index)` | 현재 위치, 시작 위치, 이번 프레임 이동량 |
| `TouchForce(index)`·`TouchHasForce(index)` | 압력과 압력 지원 여부 |
| `TouchCount()` | 지금 닿아 있는 손가락 수 |

- **포커스 상실:** `FocusLost`와 일시 정지 복귀 규칙은 키와 같다. 포커스를 잃으면 모든 손가락이 `cancelled` 끝이 된다. 복귀할 때 과거 시작 edge는 다시 발사하지 않는다.
- **큐 손실 복구:** `InputQueue`가 한 프레임에 이벤트를 버렸으면(DroppedCount 증가) 그 프레임 `BeginFrame`이 닿아 있는 모든 손가락을 `cancelled` 끝으로 만든다(입력 상세 계획 18.3절의 NeedsResync·Cancelled 규칙). 그 뒤 시작 없이 오는 `TouchMoved`·`TouchEnded`는 무시한다. 다음 `TouchBegan`부터 다시 받는다(require-release). 이 규칙이 없으면 버려진 `TouchEnded` 때문에 손가락이 눌린 채 남는다.
- **고정 크기:** `PlayerController::PlayerInput()` 스냅숏이 이 상태를 그대로 복사하므로, Pawn·Controller 코드는 마우스와 같은 방식으로 읽는다. 터치 상태는 고정 배열이라 프레임 중 할당이 없다.
- **replay:** 터치 이벤트는 입력 상세 계획 I08 replay에 기록한다. 제스처는 터치 상태에서 결정적으로 다시 계산하므로 기록하지 않는다. I08 schema에 터치 열을 더한다.

### 3.4 마우스로 터치 흉내 (데스크톱 시험)

설정 `input.useMouseForTouch`(UE `bUseMouseForTouch`, 기본 false)를 켜면 왼쪽 버튼의 누름·이동·뗌이 손가락 0번 터치의 시작·이동·끝이 된다. 이때 그 버튼의 마우스 이벤트는 만들지 않는다. UE와 같고, 같은 입력을 게임이 두 번 처리하지 않게 하기 위해서다. 다른 버튼, 휠, 왼쪽 버튼을 누르지 않은 커서 이동은 마우스 이벤트로 남는다. 모바일 게임을 데스크톱 Tryout에서 시험하는 용도다.

## 4. OS backend (PlatformWindow)

### 4.1 Windows 터치 화면·펜

- `WM_POINTERDOWN`·`WM_POINTERUPDATE`·`WM_POINTERUP`를 받는다. `GetPointerType`이 `PT_TOUCH`나 `PT_PEN`이면 터치로 만들고 `DefWindowProc`에 넘기지 않는다. 그러면 OS가 마우스 메시지를 따로 만들지 않는다. UE 기본과 같다(펜 포함 — TD6).
- `PT_MOUSE`는 기존 `WM_MOUSE*` 경로를 쓴다.
- 좌표는 `ScreenToClient`로 client 픽셀로 바꾼다.
- 압력: 터치는 `GetPointerTouchInfo`의 `pressure`(0–1024, `TOUCH_MASK_PRESSURE`가 있을 때)를, 펜은 `GetPointerPenInfo`의 `pressure`(0–1024, `PEN_MASK_PRESSURE`가 있을 때)를 0–1로 나눈다. 없으면 `hasForce = false`다.
- 취소: `WM_POINTERUP`에 `POINTER_FLAG_CANCELED`가 있으면 취소 끝이다.
  - Rocks는 `WM_POINTERCAPTURECHANGED`도 그 포인터의 취소 끝으로 본다. UE는 이 메시지를 처리하지 않는다. capture를 잃은 포인터가 눌린 채 남지 않게 하려고 더했다.
- 검증: 이벤트 변환은 Win32 메시지 값을 직접 넣는 단위 시험으로 한다. 실제 터치 화면·펜은 사람이 확인하고, 장비가 없으면 NOT RUN으로 남긴다.

### 4.2 Android 창·입력

- **진입:** UE 5.8처럼 `NativeActivity`를 상속한 Rocks Java 액티비티와 NDK `android_native_app_glue`로 한다(TD7).
- **창:** `APP_CMD_INIT_WINDOW`·`APP_CMD_TERM_WINDOW`·`APP_CMD_WINDOW_RESIZED`·`APP_CMD_GAINED_FOCUS`·`APP_CMD_LOST_FOCUS`가 `platform::Window` 상태가 된다.
  - 창 생성·파괴는 앱 수명 이벤트(`APP_CMD_PAUSE`·`APP_CMD_RESUME` 등, PlatformLayer "앱 수명 이벤트")와 별개 신호다. 백그라운드로 가면 보통 수명 이벤트 다음에 창 파괴가 오지만, 순서와 유무를 가정하지 않는다.
  - 창 파괴 때 렌더는 swapchain을 놓는다. PlatformLayer에는 이 창 신호를 수명 이벤트 옆에 따로 적는다(TI5 착수 때).
- **터치:** `AInputEvent`의 `AMotionEvent`를 읽는다.
  - `ACTION_DOWN`·`ACTION_POINTER_DOWN`은 시작이다.
  - `ACTION_MOVE`는 그 이벤트의 모든 포인터 이동이다(history 샘플은 최신 값만 쓴다. UE와 같다).
  - `ACTION_UP`·`ACTION_POINTER_UP`은 끝이고, `ACTION_CANCEL`은 모든 손가락의 취소 끝이다.
  - `ACTION_OUTSIDE`는 시작이 없던 포인터이므로 무시한다. UE는 Ended로 보내는데, Rocks에서는 시작 없는 끝이 require-release 규칙(3.3절)에 걸려 버려지므로 만들지 않는다.
  - 압력은 `AMotionEvent_getPressure`를 0–1로 자른 값이다. 장치가 압력을 보고하지 않으면(`InputDevice` 축 범위가 없으면) `hasForce = false`다.
- **그 밖의 입력:**
  - `AINPUT_SOURCE_MOUSE`는 마우스 이벤트가 된다.
  - 게임패드는 키·축 값에서 `GamepadState`를 만든다. 버튼 배치는 UE `AndroidGameControllerMapper`의 표준 배치를 따른다.
  - 키보드 키는 `AKEYCODE_*` → `input::Key` 표로 바꾼다.
- **뒤로 가기:** `input::Key`에 `Back`을 더한다(UE `EKeys::Android_Back`). `AKEYCODE_BACK`은 기본적으로 소비하고 키 이벤트만 보내며, 앱을 끝내지 않는다. UE와 같다. 끝낼지는 게임이 정한다.
- **선행:** Android APK 패키징(Gradle 프로젝트 생성·서명·`adb install`)과 Android 그래픽 backend(Vulkan 또는 OpenGL ES)가 있어야 화면이 있는 실행이 된다. 둘 다 이 문서 밖이며 6절 TI5의 선행으로만 적는다.
- **검증:** 이벤트 변환은 이벤트 값을 직접 만든 단위 시험으로 한다(NDK 빌드, 기기 없이 변환 함수만). 실제 터치는 에뮬레이터와 기기에서 사람이 확인한다.

## 5. 게임 쪽 처리

### 5.1 제스처 인식

`Gameplay`의 `PlayerController`가 입력 스냅숏 다음에 `TouchGestureRecognizer`를 돌린다. UE `UPlayerInput`이 `FGestureRecognizer`를 부르는 자리와 같다. 설정 `input.enableGestureRecognizer`(UE `bEnableGestureRecognizer`, 기본 false)가 켜져 있을 때만 돈다.

| 제스처 | 판정(UE와 같다) | 값 |
|---|---|---|
| Pinch | 손가락 0번과 1번이 모두 닿으면 시작한다. 두 점이 직전 pinch 점에서 `PinchReanchorDistanceSquared`(UE 10000 px²)를 넘게 벗어나면 기준 거리를 다시 잡는다 | 기준 거리 제곱 대비 현재 거리 제곱의 비율(시작 1.0) |
| Rotate | 손가락 0번과 1번 | 시작 각도 대비 회전 각(도) |
| Flick | 손가락 하나. `FlickMaxSeconds`(UE 0.25 s) 안에 떼고, 이동 거리 제곱이 `FlickMinDistanceSquared`(UE 10000 px²)를 넘음 | 이동 방향 각(도), 뗀 프레임에 한 번 |

- 임계값은 모두 이름 있는 상수이고 CVar로 바꿀 수 있다.
- 거리 임계는 UE처럼 픽셀 단위라 기기 DPI에 따라 체감이 다르다. Rocks는 PW2 DPI 배율이 생기면 임계에 그 배율을 곱한다(TD14). 그 전에는 배율 1.0이다.
- 제스처는 시작·진행·끝 단계를 가진 값이다. `PlayerController`에서 질의하고(`Gesture(TouchGesture::Pinch)`), 액션 매핑의 원천이 된다(5.2절).
- Tap·LongPress·Pan은 인식기에 넣지 않는다. 터치 원천과 매핑 trigger(Tap·Hold)로 만든다. UE 인식기도 이 셋을 만들지 않는다.
- 시험: 고정 delta 프레임과 합성 이벤트 열로 golden 시험한다. 판정이 프레임 수와 고정 delta로 정해지므로 시간에 기대지 않는다.

### 5.2 액션 매핑의 터치 원천

입력 상세 계획 I05–I07(액션·trigger·context·rebind, Enhanced Input 구조 — ComponentCatalog CD7·CR21)에 다음 원천을 더한다.

- `Touch0`–`Touch9`: 디지털 값(닿음)과 2D 값(위치). UE `EKeys::Touch1–10`에 대응한다. Rocks 이름은 0부터 시작하며 대응표(7절)에 적는다.
- `GesturePinch`·`GestureRotate`·`GestureFlick`: 1D 값이다.
- UI가 처리한 터치는 게임 원천이 받지 않는다(5.4절). 입력 상세 계획의 source consume 규칙(IT14)을 그대로 쓴다.

### 5.3 터치 조작 (가상 조이스틱)

- `TouchControlsAsset`(UE `UTouchInterface`)은 컨트롤 목록과 공통 값으로 이루어진다.
  - 컨트롤 하나: 버튼인지 스틱인지, 중심, 크기, 엄지 크기, 상호작용 크기, 입력 배율, 엄지·배경 이미지, 보낼 원천 둘(스틱은 가로·세로).
  - 공통 값: 활성·비활성 불투명도, 비활성까지 시간, 재중심 시간, 활성 지연, 시작 지연, 재중심 금지.
  - 위치·크기 값의 규칙은 UE `SVirtualJoystick`과 같다. 음수면 오른쪽·아래 가장자리 기준, 절댓값 1 이하면 화면 비율, 1 초과면 DPI 배율을 곱한 픽셀이다.
- 컨트롤은 게임패드 원천(왼쪽 스틱 축, South 버튼 등)을 만든다. 게임 코드는 패드와 같은 매핑으로 받는다. UE `SVirtualJoystick`이 게임패드 키를 보내는 것과 같다.
- 그리기와 터치 판정은 UI Element(`VirtualJoystickElement`)가 맡는다. 표시는 설정 `input.defaultTouchControls`와 `input.alwaysShowTouchControls`(UE `DefaultTouchInterface`·`bAlwaysShowTouchInterface`)로 정한다. 기본은 터치 플랫폼에서만 보인다.
- UI 시스템(UiToolkit UiFramework·UiScreens)과 UI 터치 라우팅(TI7), 화면 출력이 선행이다.

### 5.4 UI 터치 라우팅

- UiToolkit 3.8절 입력 라우팅에 터치를 마우스와 같은 포인터 이벤트로 넣는다. 포인터 이벤트에는 포인터 번호(손가락 0–9, 마우스 `MousePointerIndex`)와 "터치인지" 표지를 더한다.
- Element가 터치를 처리하지 않으면 같은 위치의 마우스 이벤트로 다시 보낸다(UE `bTouchFallbackToMouse`, 기본 켬, 설정 `ui.touchFallbackToMouse`).
- UI가 처리한 터치는 게임 원천에서 빠진다(5.2절).
- 이 절은 UiToolkit PW5(창별 입력 이벤트)·3.8절에 묶인다. 지금 UiToolkit 3.8절에는 터치가 없다(EngineImplementationPlan 21.4절은 3.8–3.10절에 터치가 있다고 적었다). UiToolkit은 500줄을 넘어 장별 분할 대상이라, 맡은 세션이 다음 수정 때 3.8절이 이 절을 가리키게 고친다.

### 5.5 Actor·Component 터치 이벤트

- `PlayerController`의 `enableTouchEvents`(UE `bEnableTouchEvents`, 기본 true)·`enableTouchOverEvents`(기본 false)가 켜져 있으면 손가락 위치로 화면 → 월드 판정을 한다.
- 맞은 Actor·Component에 `OnTouchBegin`·`OnTouchEnd`·`OnTouchEnter`·`OnTouchLeave`를 보낸다.
- 판정은 마우스 클릭 이벤트와 같은 경로(view 역투영 + 물리 trace)다. 그래서 카메라 view(RS0 ViewDesc)와 물리 trace가 선행이다.

## 6. 작업 순서

| 단계 | 내용 | 선행 | 검증 |
|---|---|---|---|
| TI1 | Input: 터치 이벤트 셋·`hasForce`·`InputState` 터치 질의·취소·포커스 상실·큐 손실 복구, 배정기(프레임 경계 반환), 마우스 흉내 설정, `Key::Back` | 없음 | 합성 이벤트 단위 시험: edge, 한 프레임 시작+끝, 프레임 안 번호 재사용 없음, 11번째 손가락, FocusLost, 큐 손실 뒤 require-release, 흉내 시 마우스 이벤트 없음 |
| TI2 | Windows `WM_POINTER` 터치·펜 backend, 터치 → 마우스 중복 없음, 취소 플래그·capture 상실 | TI1 | 메시지 변환 단위 시험, 실제 터치 화면·펜은 사람 확인(없으면 NOT RUN) |
| TI3 | `TouchGestureRecognizer`(Pinch·Rotate·Flick)·설정·PlayerController 질의 | TI1, ER S12(완료) | 프레임 기반 golden 시험(UE 판정 수치와 같은 경계) |
| TI4 | 액션 매핑 원천(`Touch0–9`·제스처)과 UI 소비 | TI1–TI3, TI7, 입력 I05–I07(CR21) | IT13·IT14에 터치 열 추가 |
| TI5 | Android NativeActivity 창·입력 backend(터치·마우스·키·패드·뒤로 가기), PlatformLayer 창 신호 기록 | TI1, Android APK 패키징, Android 그래픽 backend | 이벤트 변환 단위 시험(NDK), 에뮬레이터·기기 사람 확인 |
| TI6 | `TouchControlsAsset`·`VirtualJoystickElement`·표시 설정 | TI4, TI7, UiToolkit UiFramework·UiScreens, 화면 마일스톤 | UI CPU 시험(판정과 불투명도 시간은 프레임 수로), 기기 사람 확인 |
| TI7 | UI 터치 라우팅·마우스 fallback | TI1, UiToolkit PW5·3.8절 | UiToolkit 라우팅 시험에 터치 열 |
| TI8 | Actor·Component 터치 이벤트 | TI1, RS0 ViewDesc, 물리 trace | World 시험(판정은 가짜 trace 주입) |
| — | I08 replay schema에 터치 열 | TI1, I08 | IT21에 터치 열 |

진행(2026-11-24): TI1을 구현했다. `InputEventType::TouchBegan`·`TouchMoved`·`TouchEnded`(`touchIndex`·`force`·`hasForce`·`cancelled`), `MaxTouches`·`MousePointerIndex`, `Key::Back`, `input::TouchIndexAllocator`(끝난 번호는 `EndFrame`까지 잡아 둠), `InputState` 터치 질의·FocusLost 취소·`BeginFrame(events, eventsLost)` 손실 복구·require-release, `InputState::SetMouseTouchEmulation`(설정 `input.useMouseForTouch` 연결은 입력 설정이 생길 때)이다. 시험은 `Input_Touch*`·`Input_MouseTouchEmulation`·`Input_TouchIndexAllocator`, Android 에뮬레이터에서 탭·스와이프·Back을 확인했다(AndroidPlatform 9절). Android 압력 지원 판정은 후속이다(그 문서 5절).

TI2(2026-11-24): Win64 창이 `WM_POINTERDOWN`·`UPDATE`·`UP`·`CAPTURECHANGED`의 터치·펜을 `TouchIndexAllocator`로 번호를 매겨 터치 이벤트로 넘기고 `DefWindowProc`에 넘기지 않는다. 마우스 포인터는 그대로 `WM_MOUSE*`다. 메시지·플래그·압력 변환은 `PlatformWindow/Private/Common/PointerTouchMapping`(모든 플랫폼에서 시험 — `PointerTouchMapping_*`)이다. 터치 화면 없는 PC에서 `InjectTouchInput`으로 Sandbox 창에 가상 탭을 넣어 `Pawn received touch 0`·`ended`가 오고 마우스 흉내 클릭이 함께 오지 않는 것을 사람 확인했다. 실제 터치 화면·펜은 장비가 생기면 확인한다.

TI1–TI3은 화면·UI와 무관하다. 그래도 계획 순서를 지켜 입력 상세 계획의 I 단계와 함께 착수한다. 이 문서는 그때 터치가 빠지지 않게 하려는 것이다.

## 7. Rocks 이름 대응

| UE | Rocks |
|---|---|
| `ETouchIndex::Touch1–Touch10` | 손가락 번호 0–9(`MaxTouches`), 매핑 원천 `Touch0–Touch9` |
| `ETouchIndex::CursorPointerIndex` | `MousePointerIndex` |
| `OnTouchStarted`·`OnTouchMoved`·`OnTouchEnded`·`OnTouchForceChanged` | `TouchBegan`·`TouchMoved`(압력 포함)·`TouchEnded` |
| `UE::FTouchState` | `InputState`의 손가락별 상태 |
| `FGestureRecognizer`·`EKeys::Gesture_*`·`bEnableGestureRecognizer` | `TouchGestureRecognizer`·`GesturePinch` 등·`input.enableGestureRecognizer` |
| `EKeys::Android_Back` | `input::Key::Back` |
| `UTouchInterface`·`SVirtualJoystick` | `TouchControlsAsset`·`VirtualJoystickElement` |
| `bUseMouseForTouch`·`bTouchFallbackToMouse` | `input.useMouseForTouch`·`ui.touchFallbackToMouse` |
| `DefaultTouchInterface`·`bAlwaysShowTouchInterface` | `input.defaultTouchControls`·`input.alwaysShowTouchControls` |
| `bEnableTouchEvents`·`bEnableTouchOverEvents` | `enableTouchEvents`·`enableTouchOverEvents` |

## 8. 결정

| ID | 결정 | 근거 |
|---|---|---|
| TD1 | 손가락 10개, 마우스 포인터 번호 1개는 따로 | UE `NUM_TOUCH_KEYS = 11`, `CursorPointerIndex`와 같다 |
| TD2 | OS 포인터 ID를 그대로 쓰지 않고 배정기로 0–9를 준다(Android 포함). 끝난 번호는 프레임 경계에서 돌려받는다 | Windows ID는 작은 수가 아니다. 규칙을 하나로 두고, 한 프레임 안 재사용으로 edge·시작 위치가 깨지는 것을 막는다. UE Android는 `pointerId`를 그대로 쓰지만 결과(0부터 작은 번호)는 같다 |
| TD3 | 압력 변화는 `TouchMoved`에 합친다. `OnTouchFirstMove`는 이벤트로 두지 않는다 | 첫 이동은 UI가 `TouchStartPosition`과 비교해 끌기 판정으로 얻는다. 이벤트 종류가 준다 |
| TD4 | 끝 이벤트에 `cancelled` 표지를 둔다 | UE는 CANCEL을 Ended로 합친다. Rocks도 Ended로 두되, 게임이 탭 완료와 취소를 가를 수 있게 표지를 남긴다(입력 상세 계획의 Cancelled 규칙과 맞춘다) |
| TD5 | Windows는 `WM_POINTER` | UE 기본과 같고, 터치가 마우스 메시지를 만들지 않는다 |
| TD6 | 펜은 `WM_POINTER`로 받아 터치로 보낸다. 기울기·지우개는 범위 밖 | UE 기본(`UsePointerMessagesForPen = true`)과 같다 |
| TD7 | Android 진입은 NativeActivity 상속 액티비티 + `android_native_app_glue` | UE 5.8과 같다(UE `GameActivity`는 NativeActivity를 상속한 자체 클래스). AndroidX games-activity는 UE와 달라 쓰지 않는다. 텍스트 입력(IME)은 PW3에서 Java 쪽을 더한다 |
| TD8 | 제스처 인식은 Pinch·Rotate·Flick만, 기본 꺼짐. Tap·Hold는 매핑 trigger | UE 인식기·기본값과 같다 |
| TD9 | 가상 조이스틱은 게임패드 원천을 만든다 | UE와 같다. 게임 코드가 패드와 터치를 따로 다루지 않는다 |
| TD10 | UI 미처리 터치는 마우스로 다시 보낸다(기본 켬) | UE `bTouchFallbackToMouse` 기본 true |
| TD11 | 마우스로 터치 흉내는 기본 꺼짐. 켜면 손가락 0번을 쓰고 그 버튼의 마우스 이벤트는 만들지 않는다 | UE와 같다. 같은 입력을 두 번 처리하지 않는다 |
| TD12 | OS 제스처 이벤트(`OnTouchGesture` — 트랙패드 확대·회전·스크롤)는 이번 범위 밖 | 데스크톱 트랙패드 기능이고 Android 터치와 무관하다. 필요하면 PW5에 더한다 |
| TD13 | 압력은 OS 값을 넘기고 `hasForce`로 지원 여부를 표시한다 | UE는 1.0 고정이다. 값이 있는데 버릴 이유가 없고, 표지로 "가짜 값" 문제를 피한다. API 모양은 UE(`Force` 인자)와 같다 |
| TD14 | 제스처 거리 임계에 DPI 배율(PW2)을 곱한다 | UE는 픽셀 고정이라 고밀도 화면에서 판정이 달라진다. 배율 1.0일 때 UE와 같다 |
| TD15 | Android `ACTION_OUTSIDE`는 무시한다 | 시작이 없는 포인터다. UE는 Ended로 보내지만 Rocks require-release 규칙에서는 버려지므로 만들지 않는다 |
| TD16 | 큐 손실 때 모든 손가락을 취소 끝으로 하고 require-release | 입력 상세 계획 18.3절 규칙. 버려진 끝 이벤트로 손가락이 눌린 채 남는 것을 막는다 |

## 9. 남는 한계

- 펜 기울기·지우개·hover, iOS, 접근성 터치 탐색은 다루지 않는다.
- Android 패키징과 그래픽 backend가 없어 TI5는 그 둘이 생길 때까지 시작할 수 없다.
- 다중 창 터치 대상은 PW1·PW5가 정해질 때 확정한다. 그 전에는 창 하나다.
- 실제 장치 확인(Windows 터치 화면·펜, Android 기기)은 자동 시험이 아니다. 장비가 없으면 NOT RUN으로 남긴다.
