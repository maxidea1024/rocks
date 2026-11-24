# Android 실행 기반 — 앱 진입·창·수명·APK·입력

개정 3 (2026-11-24, 구현 착수: Java·Gradle·glue 없이 — AD1·AD10·AD11). ShaderSystem 선행 P13(`ShaderSystem/05-Prerequisites.ko.md`)의 설계다. 입력 변환은 [터치 입력](TouchInput.ko.md) 4.2절이 정하고, 이 문서는 그 밖의 실행 기반을 정한다. AP1–AP4 구현은 2026-11-24에 착수했다(9절).

## 1. 목적과 범위

mobile은 Android를 먼저 한다(PlatformLayer "모바일 플랫폼"). 지금 Android에서 되는 일은 다음뿐이다.

- 시험 실행: NDK 빌드한 native 실행 파일을 `adb`로 올려 돌린다.
- 교차 빌드: Vulkan·OpenGL ES backend가 컴파일만 된다.

앱으로 실행할 수 없고, 창·입력·APK도 없다. 그래서 `PlatformWindow`는 Unsupported다.

범위:
- 앱 진입: 플랫폼 `NativeActivity`, Launch의 `ANativeActivity_onCreate`, Launch 연결.
- 스레드: 게임 스레드와 이벤트 스레드.
- 창: `ANativeWindow` 수명, 크기, 포커스. 창 파괴 때 렌더와 맞추는 gate.
- 앱 수명 이벤트를 PlatformLayer 큐로 잇는다.
- 입력: TouchInput 4.2절 변환을 펌프에 연결.
- 로그: logcat sink.
- 경로·명령줄.
- APK 패키징: Gradle 프로젝트 생성, 서명, 설치, 실행. 쿠킹 데이터를 APK 안에 넣는다.
- APK 안 파일 읽기는 AssetSystem VFS(APK)의 계약만 정한다.

범위 밖:
- iOS
- Play 배포 형식(AAB·Play Asset Delivery), OBB
- IME(PW3 — Java `InputMethodManager`)
- 화면 잘림 영역(display cutout)·safe area(UI 단계)
- 센서
- 프레임 페이싱 라이브러리(Swappy)
- 크래시 수집(CrashHandlingAndLaunch의 Android 절이 정한다)

## 2. UE 5.8 기준

경로는 `C:\github\UnrealEngine\Engine\Source` 기준이다.

| 항목 | UE 5.8 | 내용 |
|---|---|---|
| 액티비티 | `Programs/UnrealBuildTool/Platform/Android/UEDeployAndroid.cs:6057` | 자체 Java 클래스 `GameActivity`가 `NativeActivity`를 상속한다 |
| native 진입 | `Runtime/Launch/Private/Android/LaunchAndroid.cpp:20, 1026–1055` | `android_native_app_glue`의 `android_main`이 엔진(게임 스레드)을 돈다 |
| 이벤트 스레드 | `LaunchAndroid.cpp:954–1000` | 별도 "EventWorker" 스레드가 looper를 만들고 `onAppCmd = OnAppCommandCB`, `onInputEvent = HandleInputCB`를 붙여 앱 명령·입력을 처리한다. 게임 스레드는 `FAppEventManager` 큐를 tick에서 꺼낸다(`:781`) |
| 앱 명령 | `LaunchAndroid.cpp:1692–1959` | SAVE_STATE·INIT_WINDOW·TERM_WINDOW·LOST/GAINED_FOCUS·WINDOW_RESIZED·CONFIG_CHANGED·LOW_MEMORY·START·RESUME·PAUSE·STOP·DESTROY |
| 창 파괴 gate | `LaunchAndroid.cpp:1588–1650, 1745–1768` | TERM_WINDOW에서 이벤트 스레드가 게임 스레드에 콜백(비활성·백그라운드 delegate, 오디오 정지)을 넣고 완료 trigger를 기다린 뒤 렌더를 막는다. `android_app->window`는 이 처리 뒤 null이 된다 |
| APK 안 파일 | `Runtime/Core/Private/Android/AndroidPlatformFile.cpp:310–319, 443` | `AAssetManager`로 열고, `AAsset_openFileDescriptor64`로 비압축 항목을 fd·오프셋으로 읽는다 |
| 데이터 배치 | `UEDeployAndroid.cs:65–67, 127, 315–329` | `bPackageDataInsideApk`면 쿠킹 데이터를 APK 안에, 아니면 OBB에 둔다. 빌드는 Gradle, AAB는 bundletool |
| EGL | OpenGLESGraphics 문서(`Android/AndroidEGL.cpp`) | 모든 표면이 EGL context 하나를 공유한다. 창이 없으면 pbuffer를 쓴다 |

## 3. 앱 진입과 스레드

개정 3(2026-11-24, 구현 착수): Java 코드와 `android_native_app_glue` 없이 시작한다(AD1·AD10).

- **액티비티:** manifest가 플랫폼 기본 `android.app.NativeActivity`를 쓰고 `android.app.lib_name`으로 `lib<Program>.so`를 고른다(`android:hasCode="false"`).
  - Java가 맡던 일은 JNI로 한다. `ANativeActivity_onCreate`(UI 스레드, `activity->env`)에서 다음을 읽는다.
    - 앱 저장소 경로: `internalDataPath`와 `getCacheDir()`. 이 둘로 `RegisterApplicationDirectories`를 부른다.
    - 명령줄: debuggable 앱이면 Intent extra `rocks.args`(문자열 배열)(AD7).
  - Java 클래스는 그것이 꼭 필요한 기능이 올 때 더한다: 오디오 포커스 콜백(AU9), IME(PW3).
- **native 진입:** Launch가 `ANativeActivity_onCreate`를 정의한다(Launch `Private/Android`).
  - 빌드 규칙의 `AndroidPackage`가 그 실행 파일 모듈에 define `ROCKS_ANDROID_APK=1`을 준다. 그러면 `EntryPoint.h`는 `main` 대신 앱 기술(`Describe`)을 Launch에 넘기는 export 함수 `RocksAndroidApplicationDescription`을 만든다.
  - 콘솔 시험 실행 파일(adb로 올리는 것)은 지금처럼 `main`이다(AD5).
  - APK 프로그램은 공유 라이브러리 `lib<Program>.so`다. 링크에 `-shared`·`-Wl,-u,ANativeActivity_onCreate`·`-Wl,-z,max-page-size=16384`를 준다. APK 프로그램은 Monolithic만 받는다.
- **스레드(AD2):**
  - UI 스레드는 `ANativeActivity` 콜백(창·포커스·입력 큐·수명)을 받는다. 콜백은 게임 상태를 만지지 않고 다음만 한다.
    - 등록된 창 listener(PlatformWindow Android backend)에 알린다.
    - 수명 표시를 남긴다.
    - `onNativeWindowDestroyed`에서 창 파괴 gate를 기다린다(4절).
  - 입력 스레드는 PlatformWindow Android backend가 띄운다. "InputThread"가 looper를 만들고 `AInputQueue_attachLooper`로 입력 큐를 붙여 입력 이벤트를 읽는다(5절).
  - 게임 스레드는 Launch가 `onCreate`에서 `platform::Thread`("GameThread", 8 MB 스택 — 이름 있는 상수)로 띄워 기존 Launch 실행(`RunApplication` — 단계·본문)을 돈다.
  - UE는 glue 스레드에서 엔진을 돌고 별도 "EventWorker"가 looper를 돈다(`LaunchAndroid.cpp:954–1000`). Rocks는 glue를 쓰지 않으므로, 콜백을 UI 스레드에서 직접 받고 looper가 필요한 입력만 입력 스레드에 둔다.
- **종료:**
  - 게임 스레드의 본문이 끝나면 `ANativeActivity_finish`를 부른다.
  - `onDestroy`(UI 스레드)는 게임 스레드에 종료를 요청하고(`launch::RequestTermination`) 게임 스레드를 join한다. 그다음 `platform::ExitProcessWithoutTeardown(exitCode)`로 프로세스를 끝낸다(AD8 — 같은 프로세스에서 `onCreate`가 다시 불릴 수 있는데 Rocks 정적 상태는 재진입을 전제로 하지 않는다).
  - `onDestroy` 전에 본문이 끝났으면 join은 바로 끝난다.
## 4. 창과 렌더 surface

`platform::Window`의 Android 구현은 "앱의 그 창 하나"다. `Window::Create`는 앱이 하나만 만들 수 있고, `WindowDesc`의 크기·제목은 무시한다(전체 화면). 제목은 최근 앱 목록 이름이며 `SetTitle`은 Unsupported다.

| `ANativeActivity` 콜백(UI 스레드) | 처리 |
|---|---|
| `onNativeWindowCreated` | `ANativeWindow`를 잡고(`ANativeWindow_acquire`) 크기를 갱신한다. `NativeHandle().window`가 그 포인터가 되고 "창 있음" 세대가 올라간다 |
| `onNativeWindowDestroyed` | 창 파괴 gate(아래) 뒤 `ANativeWindow_release`, "창 없음". 콜백이 돌아오면 OS가 surface를 없앤다 |
| `onNativeWindowResized`·`onConfigurationChanged`(회전) | 크기 갱신, `ConsumeResize`가 알린다. 회전·크기 변경에 액티비티가 다시 만들어지지 않게 manifest 틀에 `android:configChanges`(orientation·screenSize·screenLayout·keyboardHidden 등, UE `UEDeployAndroid.cs:2833`)를 둔다 |
| `onWindowFocusChanged` | `HasFocus`. 잃으면 `FocusLost`(모든 손가락 취소 — TouchInput 3.3절) |
| `onPause`·`onResume` | 앱 수명 `EnteredBackground`·`EnteredForeground`(앱 수명 큐는 AU9에서 연결 — 지금은 로그만) |
| `onLowMemory` | 앱 수명 `LowMemory`(PlatformLayer 이벤트 목록, 연결은 위와 같다) |
| `onInputQueueCreated`·`onInputQueueDestroyed` | 입력 스레드 looper에 붙이고(`AInputQueue_attachLooper`) 뗀다(5절) |
| `onDestroy` | `WillTerminate`, 종료 요청 → 게임 스레드 join → 프로세스 종료(3절) |

- **창 파괴 gate(AD3):** OS는 `onNativeWindowDestroyed`가 돌아오면 surface를 없앤다. 그 전에 렌더가 그 surface를 놓아야 한다.
  1. 이벤트 스레드가 창 큐에 `SurfaceLost` 요청을 넣는다.
  2. 이벤트 스레드는 gate(`platform::ManualResetEvent`)를 기다린다.
  3. 게임 스레드는 다음 `PumpEvents`에서 이를 보고 호스트에 알린다. 호스트는 렌더에 swapchain·EGL window surface를 놓게 하고, 진행 중인 GPU 작업이 그 surface를 쓰지 않게 한다.
  4. 게임 스레드가 `Window::AcknowledgeSurfaceLost()`로 gate를 연다.
  5. UI 스레드가 그 뒤 `ANativeWindow_release`를 하고 `onNativeWindowDestroyed`에서 돌아온다.
  - 기다리는 동안 Java UI 스레드가 막혀 있다(이 대기는 UI 스레드 콜백 안이다). 그래서 UE 옛 창 동작처럼 제한 시간을 둔다: `SurfaceReleaseTimeoutMilliseconds`(= 4000, UE `EMDoneTrigger->Wait(4000)` — `LaunchAndroid.cpp:1654–1667`).
    - 넘으면 Error 로그를 남기고 진행한다. 렌더는 surface 상실 오류(`VK_ERROR_SURFACE_LOST_KHR`, `EGL_BAD_SURFACE`)를 정상 경로로 처리해야 한다.
    - 게임 스레드가 이미 끝났거나 끝나는 중이면 gate를 바로 연다.
    - 제한 시간은 ANR 방지 장치다. 시험은 이 시간에 기대지 않고, 명시 gate와 0 제한 시간 주입으로 두 경로를 결정적으로 확인한다.
  - UE 새 창 동작은 이벤트 스레드에서 RHI release 콜백을 바로 부른다(`:1567–1585`, `:1757–1764`). Rocks는 렌더 자원을 게임·렌더 쪽만 만지게 하려고 gate 방식을 쓴다.
- **첫 프레임:** Android는 창 surface에 프레임이 한 번도 오지 않으면 창을 그려지지 않은 것으로 보고 입력을 보내지 않는다(`InputDispatcher` "NO_INPUT_CHANNEL" → 입력이 쌓이면 ANR). 렌더러가 없는 호스트(지금 Sandbox)는 새 surface마다(`ConsumeResize`가 true — 새 surface는 크기가 같아도 true) `Window::PresentBlankFrame()`으로 CPU 검은 프레임 한 장을 낸다. CPU로 연결한 surface에는 GPU swapchain을 만들 수 없으므로 렌더러가 있는 호스트는 부르지 않는다.
- **surface 다시 만들기:** 창이 다시 생기면(`onNativeWindowCreated`) `SurfaceAvailable`을 알리고 렌더가 swapchain을 다시 만든다. 창이 없는 동안 엔진은 돌되 present를 건너뛴다(`IsMinimized`가 true — 데스크톱 최소화와 같은 계약).
- **그래픽 연결:**
  - OpenGL ES: EGL 플랫폼 층이 `NativeHandle().window`로 window surface를 만든다. 창이 없으면 pbuffer다(OpenGLESGraphics 문서 그대로).
  - Vulkan: `VK_KHR_android_surface`로 만든다(VulkanGraphics 문서의 플랫폼 surface).

## 5. 입력 펌프

- 입력 스레드의 looper는 `ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS)`로 만든다. 입력 큐를 콜백 없이 ident로 등록하므로, 이 플래그가 없으면 `AInputQueue_attachLooper`의 fd 등록이 실패하고 입력이 끝나지 않아 ANR이 된다(UE `LaunchAndroid.cpp:970`과 같다 — 2026-11-24 에뮬레이터에서 확인).
- 입력 스레드가 looper에서 `AInputQueue_getEvent`·`AInputQueue_preDispatchEvent`로 꺼낸 `AInputEvent`를 TouchInput 4.2절 규칙으로 `input::InputEvent`로 바꿔 창 큐에 넣는다. 터치 번호 배정기는 게임 스레드의 `EndInputFrame`이 프레임 경계를 알릴 때까지 끝난 번호를 잡아 둔다.
- `AInputQueue_finishEvent`에 처리한 이벤트는 1, 처리하지 않은 것은 0을 준다. 볼륨 키는 0이어서 OS가 처리한다. Back은 1(소비 — TouchInput 4.2절)이다.
- `Window::PumpEvents`(게임 스레드)는 데스크톱과 같은 모양으로 큐를 호스트 `InputQueue`로 옮긴다. 넘쳐서 버린 수는 손실로 센다.
- **터치(TI1 반영, 2026-11-24):** `ACTION_DOWN`·`POINTER_DOWN`은 `TouchIndexAllocator`가 `pointerId`에 번호를 배정해 `TouchBegan`, `MOVE`는 그 이벤트의 모든 포인터의 `TouchMoved`, `UP`·`POINTER_UP`은 `TouchEnded`, `CANCEL`은 닿아 있던 모든 손가락의 취소 끝이다. 배정기의 프레임 경계는 `PumpEvents`다. 압력은 `AMotionEvent_getPressure`를 0–1로 자른 값이다. `hasForce`는 입력 장치마다 한 번 `InputDevice.getDevice(id).getMotionRange(AXIS_PRESSURE)`를 JNI로 물어(입력 스레드를 JavaVM에 붙인다) 범위가 있고 폭이 0보다 크면 참이다. 결과는 장치 ID별로 8칸까지 기억한다. adb 주입 이벤트(장치 -1)는 거짓으로 판정되는 것을 확인했고, 실제 터치 장치는 사람 확인이 남았다. 마우스 소스(`AINPUT_SOURCE_MOUSE`)는 마우스 이벤트다. Back은 `Key::Back`으로 넘기고 소비한다.
- 게임패드(2026-11-24 구현): 입력 스레드가 `AINPUT_SOURCE_GAMEPAD`·`JOYSTICK`의 버튼 키와 축을 프로세스 공용 패드 저장소에 모으고, `GamepadBackend::Poll`(게임 스레드)이 바뀐 패드만 `GamepadState`로 낸다(XInput과 같은 계약). 폴링 사이에 눌렀다 뗀 버튼은 "눌림 → 현재" 두 상태로 내 edge를 잃지 않는다. 배치는 UE `AndroidGameControllerMapper` 표준이다: A·B·X·Y → South·East·West·North, L1·R1, SELECT → Back, START, THUMBL·THUMBR, DPAD 키·HAT 축 → 방향 버튼, 스틱 X·Y와 Z·RZ(Y는 뒤집음), 트리거는 LTRIGGER·BRAKE와 RTRIGGER·GAS 중 큰 값. 슬롯은 장치 ID 순서다. 연결 해제 감지는 `InputManager` 리스너(Java)가 필요해 남았다 — 그때까지 앉은 패드는 연결된 채다. 에뮬레이터에서 `adb shell input gamepad keyevent KEYCODE_BUTTON_A/B`가 South·East로 Pawn까지 오는 것을 확인했다.

## 6. 로그·경로·명령줄

- **로그:** APK에는 stderr를 보는 사람이 없다. LogHost가 Android에서 stderr sink 옆에 logcat sink(`__android_log_write`, tag "Rocks", Rocks 수준 → `ANDROID_LOG_*`)를 더 단다. adb 시험 실행 파일은 stderr를 그대로 읽는다. Fatal 비상 경로도 logcat에 쓴다.
  - 한 줄 길이 제한(약 4 KB)을 넘으면 나눠 쓴다(이름 있는 상수).
  - 시험 실행 파일(adb shell)은 지금처럼 stderr다.
- **경로:** 3절의 `RegisterApplicationDirectories`가 `UserData`·`UserCache`를 정한다(PlatformLayer에 이미 있는 계약).
- **명령줄:** Intent extra `rocks.args`를 UTF-8 인자 배열로 받아 Launch의 명령줄 캡처에 넣는다. 개발 실행은 `adb shell am start -n <package>/android.app.NativeActivity --esa rocks.args --engine,--window` 꼴이다. extra가 없으면 앱 기술의 기본 인자(Sandbox는 `--engine --window`)를 쓴다.

## 7. APK 패키징 (Rocks.Build)

개정 3(2026-11-24): Gradle 없이 SDK 도구로 만든다(AD11). Java 코드가 없으니 dex 단계도 없다.

- **빌드 규칙:** 실행 파일 모듈 규칙에 `AndroidPackage = new("com.rocks.sandbox")`(패키지 이름)를 둔다. Android 대상에서만 받으며, 다른 대상·정적 라이브러리·Modular이면 입력 검증 오류다.
- **링크:** `bin/lib<Program>.so`를 만든다(3절 인자).
- **패키징(빌드 뒤 Rocks.Build.Cli):**
  1. `AndroidManifest.xml`을 만든다. manifest 값은 다음과 같다.
     - `minSdkVersion`은 Toolchain의 Android API, `targetSdkVersion`은 설치된 가장 높은 platform이다.
     - `NativeActivity`, `android:configChanges`(회전·크기·키보드 등, UE `UEDeployAndroid.cs:2833`), `android:extractNativeLibs="false"`.
     - LAUNCHER intent filter를 단다. Debug·Development는 debuggable이다.
  2. `aapt2 link -I <platform>/android.jar --manifest …`로 기본 APK를 만든다.
  3. `lib/<abi>/lib<Program>.so`를 비압축(stored)으로 넣는다.
  4. 쿠킹 데이터가 있으면 `assets/`에 낱개·비압축으로 넣는다(AD4. 지금 Sandbox는 없다).
  5. `zipalign -P 16 -f 4`로 정렬한다(16 KB 페이지의 `.so` 직접 mmap).
  6. `apksigner`로 서명한다. Debug는 `~/.android/debug.keystore`(없으면 JDK `keytool`로 만든다), Release는 설정 keystore다(배포 단계).
- **도구:** Android SDK(`build-tools`·`platforms`, `ANDROID_HOME`·`ANDROID_SDK_ROOT`·기본 위치)와 JDK(`JAVA_HOME` 또는 PATH — apksigner·keytool). 없으면 설치 원칙대로 설치한다.
- **ABI:** 지금은 빌드 하나가 ABI 하나다(`--android-abi`). 여러 ABI를 한 APK에 넣는 것은 배포 단계다.
- **설치·실행(개발):** `adb install -r <apk>`, `adb shell am start -n <package>/android.app.NativeActivity --esa rocks.args --engine,--window`, `adb logcat -s Rocks`다. 에뮬레이터는 x86_64 AVD다.
- **APK 안 파일(VFS):** `platform::FileSystem`의 Android APK backend가 `AAssetManager`로 읽기 전용 열기·크기·읽기를 한다. 비압축 항목은 `AAsset_openFileDescriptor64` fd·오프셋으로 비동기 읽기를 한다(UE와 같다). 구현은 AssetSystem "VFS(APK)" 후속 항목이 맡고, 이 문서는 계약만 정한다.
## 8. 검증

이 PC에서 NDK·에뮬레이터로 할 수 있다.

| 검증 | 방법 |
|---|---|
| 단위 시험 | 액티비티 콜백 → 상태 전이, 창 파괴 gate(게임 스레드가 늦게 확인해도 순서가 지켜지고, 게임 스레드가 끝났으면 바로 열림), 입력 변환, logcat 줄 나누기를 Android 없이 도는 순수 함수와 가짜 glue로 시험한다(EngineTests, Windows에서도 돈다). 순서는 명시 gate로 정하고 시간에 기대지 않는다 |
| 기기 단위 시험 | 기존처럼 `-t:Test -p:RocksPlatform=android`(adb 실행 파일)로 Android 전용 변환·경로 시험 |
| APK 실행 | Sandbox APK를 에뮬레이터에 설치·실행한다. logcat에서 엔진 시작·"Opened world Main"·종료 줄을 확인한다. 홈 버튼 → 복귀(TERM/INIT_WINDOW) 뒤에도 계속 도는지 확인한다 |
| 사람 확인 | 에뮬레이터·기기에서 터치·멀티터치·회전·Back·백그라운드 복귀 |

## 9. 작업 순서

| 단계 | 내용 | 선행 |
|---|---|---|
| AP1 | Launch Android 진입(`ANativeActivity_onCreate`·게임 스레드·종료·창 listener 등록), logcat sink, 명령줄·경로 | 없음 |
| AP2 | PlatformWindow Android 창·앱 명령·창 파괴 gate·수명 이벤트 큐 연결 | AP1 |
| AP3 | APK 패키징(`package-android`·Gradle 틀·서명·`Run`), 도구 설치 | AP1 |
| AP4 | Sandbox APK(Android용 Sandbox 구성: GLES 또는 Vulkan만 링크), 에뮬레이터 실행 확인 | AP2, AP3, P11·P9 Android 실행 |
| AP5 | 입력(TouchInput TI5)·게임패드 | AP2, TouchInput TI1 |
| AP6 | APK VFS(AssetSystem 후속) | AP3 |

진행(2026-11-24, x86_64 에뮬레이터 API 35에서 확인):
- AP1·AP3·AP4: `Sandbox.apk`(Gradle 없이 aapt2·zipalign·apksigner)가 설치·실행되고, 게임 스레드에서 엔진이 돌며 로그가 logcat(tag Rocks)으로 나온다. extra 없이 띄우면 `--engine --window`다.
- AP2: 창 listener·입력 스레드·창 파괴 gate(호스트 확인)·빈 프레임. 홈 → 복귀(pause·stop·start·resume)에서 gate 시간 초과와 ANR이 없다.
- AP5: 터치(TI1)·Back·키·게임패드 버튼이 Pawn까지 온다. 패드 연결 해제 감지(Java 리스너)만 남았다.
- AP6(APK VFS)은 남았다.

ShaderSystem P13이 막는 SH6(Android 셰이더·그래픽 실행)은 AP4까지 끝나면 시작할 수 있다.

## 10. 결정

| ID | 결정 | 근거 |
|---|---|---|
| AD1 | 지금은 Java 없이 플랫폼 `NativeActivity`와 JNI(경로·인자). Java 액티비티는 오디오 포커스(AU9)·IME(PW3) 때 더한다 | UE는 자체 Java `GameActivity`(NativeActivity 상속)를 쓰지만, 지금 Java가 맡을 일이 없다. AndroidX games-activity는 쓰지 않는다(TouchInput TD7) |
| AD2 | UI 스레드(콜백)·입력 스레드(looper)·게임 스레드(새 스레드, 8 MB 스택)를 나눈다 | UE처럼 OS 이벤트와 게임 프레임을 나눠 게임 프레임이 길어도 OS 콜백이 막히지 않게 한다. glue가 없으므로 looper가 필요한 입력만 따로 둔다 |
| AD3 | 창 파괴는 게임 스레드 확인 gate 뒤에 놓고, 제한 시간(4초) 뒤에는 진행한다 | OS가 `onNativeWindowDestroyed` 뒤 surface를 없애므로 렌더가 먼저 놓아야 한다. UE 옛 창 동작도 4초 제한으로 기다린다. 무제한 대기는 UI 스레드를 막아 ANR이 된다 |
| AD4 | 쿠킹 데이터는 APK 안, 낱개 파일, 비압축 | APK 안에 두는 것은 UE `bPackageDataInsideApk`와 같다. 묶음 파일(UE `main.obb.png`) 대신 낱개인 것은 쿠킹 매니페스트가 파일 단위이기 때문이다. OBB·AAB는 배포 단계 |
| AD5 | 콘솔 시험 실행 파일은 `main`을 유지하고, APK 프로그램만 `ANativeActivity_onCreate` 진입 | 지금의 adb 시험 경로를 깨지 않는다 |
| AD6 | Android 기본 로그 sink는 logcat | stderr를 보는 사람이 없다. UE도 logcat으로 낸다 |
| AD7 | 명령줄 extra는 debuggable 빌드에서만 받는다 | Release에서 받으면 외부 앱이 명령줄을 주입할 수 있다 |
| AD8 | `onDestroy`에서 게임 스레드 join 뒤 프로세스를 끝낸다 | 같은 프로세스에서 `onCreate`가 다시 불릴 수 있는데 Rocks 정적 상태는 재진입을 전제로 하지 않는다 |
| AD10 | `android_native_app_glue` 대신 `ANativeActivity` 콜백을 Launch가 직접 받는다 | glue는 ThirdParty 반입(출처·해시)이 필요하고, glue 스레드가 콜백을 다른 스레드로 넘기는 구조는 Rocks 배치(게임 스레드 분리·제한 시간 있는 창 gate)와 겹친다. looper가 필요한 입력만 입력 스레드에 둔다 |
| AD11 | APK는 Gradle 없이 SDK 도구(aapt2·zipalign·apksigner)로 만든다 | Java 코드가 없어 Gradle·Android Gradle Plugin(네트워크 내려받기·느린 시작)이 필요 없다. UE는 Gradle을 쓴다. Java가 들어오면 javac·d8 단계를 더한다 |
| AD9 | `Window`의 Android 계약 변경(크기 무시 `Create`·세대가 바뀌는 `NativeHandle`·`AcknowledgeSurfaceLost`·surface 알림)은 AP2에서 `Window.h`에 적는다 | 데스크톱 계약(`Window.h:46–47` — 창보다 오래 보관하지 않는 고정 핸들)과 달라지는 부분을 공개 헤더에 명시해야 한다 |
