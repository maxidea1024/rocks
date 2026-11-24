# Android 실행 기반 — 앱 진입·창·수명·APK·입력

개정 2 (2026-11-24, 개정 1에 UE·API 교차 검증 반영). ShaderSystem 선행 P13(`ShaderSystem/05-Prerequisites.ko.md`)의 설계다. 입력 변환은 [터치 입력](TouchInput.ko.md) 4.2절이 정하고, 이 문서는 그 밖의 실행 기반을 정한다. 설계만 하며, 착수는 계획 순서(SH6 전)를 따른다.

## 1. 목적과 범위

mobile은 Android를 먼저 한다(PlatformLayer "모바일 플랫폼"). 지금 Android에서 되는 일은 다음뿐이다.

- 시험 실행: NDK 빌드한 native 실행 파일을 `adb`로 올려 돌린다.
- 교차 빌드: Vulkan·OpenGL ES backend가 컴파일만 된다.

앱으로 실행할 수 없고, 창·입력·APK도 없다. 그래서 `PlatformWindow`는 Unsupported다.

범위:
- 앱 진입: Java 액티비티, `android_main`, Launch 연결.
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

- **Java:** `RocksActivity extends NativeActivity` 하나를 둔다. 하는 일은 셋이다.
  - `onCreate`에서 앱 저장소 경로(`getFilesDir`·`getCacheDir`)를 JNI로 넘긴다(`RegisterApplicationDirectories` — PlatformLayer).
  - 명령줄은 Intent extra `rocks.args`(문자열 배열)로 받는다. 단, debuggable 빌드에서만 받는다. Release에서 받으면 외부에 노출된 액티비티를 통해 다른 앱이 명령줄을 넣을 수 있다(AD7).
  - `AudioManager` 포커스 콜백을 등록한다(PlatformLayer 앱 수명 — 오디오 AU9 때 쓴다).
  - native 라이브러리 이름은 manifest `android.app.lib_name`으로 정한다. 그 밖의 로직은 Java에 두지 않는다(AD1).
- **native 진입:** `android_main(android_app*)`은 Launch의 Android 진입(`launch::RunAndroidApplication`)이다.
  - 빌드 규칙의 `AndroidPackaging = Apk`가 대상 define `ROCKS_ANDROID_APK=1`을 준다. `EntryPoint.h`는 그 define이 있으면 `main` 대신 `android_main`을 만든다.
  - 콘솔 시험 실행 파일(adb로 올리는 것)은 지금처럼 `main`이다(AD5).
  - APK 프로그램은 공유 라이브러리 `lib<Program>.so`다. glue가 정적 라이브러리라서 링크에 `-u ANativeActivity_onCreate`를 준다. APK 프로그램은 Monolithic만 받는다. Modular(`ROCKS_MODULE_SHARED_LIBRARY`)는 `.so` 여러 개라 이 문서 범위 밖이고, 규칙 검증이 거부한다.
- **스레드(AD2):** UE처럼 OS 이벤트와 게임 프레임을 다른 스레드로 나눈다. 다만 역할 배치는 UE와 반대다.
  - glue 스레드(`android_main`)가 이벤트 스레드다. glue가 이미 그 스레드에 looper(`state->looper`)와 input queue를 붙여 두므로, UE처럼 looper를 다른 스레드로 옮길 필요가 없다. UE는 새 "EventWorker"에서 `ALooper_prepare`·`ALooper_addFd(msgread)`·`state->looper` 교체를 `APP_CMD_INPUT_CHANGED` 전에 해야 한다(`LaunchAndroid.cpp:970–979`).
  - 게임 스레드는 Launch가 `platform::Thread`("GameThread")로 새로 띄운다. 스택은 데스크톱 주 스레드와 같은 8 MB(이름 있는 상수)다. glue 스레드의 pthread 기본 스택(약 1 MB)으로는 지금 본문을 감당하지 못한다.
  - 이벤트 스레드는 `ALooper_pollOnce`로 앱 명령(`onAppCmd`)·입력(`onInputEvent`)을 처리한다. 게임 상태는 만지지 않는다. 할 수 있는 일은 둘뿐이다.
    - 고정 크기 큐에 넣는다: 입력 이벤트는 창 큐, 수명 이벤트는 PlatformLayer 큐.
    - 창 파괴 gate를 기다린다(4절).
- **종료:**
  - 게임 스레드의 `Run`이 끝나면 `ANativeActivity_finish`를 요청한다.
  - 이벤트 스레드(glue)는 `APP_CMD_DESTROY`(`state->destroyRequested`)까지 계속 돈다. glue의 `onDestroy`가 `android_main`이 돌아올 때까지 UI 스레드를 막고, 그 사이 오는 TERM_WINDOW를 처리할 스레드가 있어야 하기 때문이다(UE도 종료 요청까지 이벤트 루프를 돈다 — `:996`).
  - 반대로 `APP_CMD_DESTROY`가 먼저 오면 `WillTerminate`이자 종료 요청(`launch::RequestTermination`)이다. 게임 스레드가 끝날 때까지 기다린 뒤 join한다.
  - 같은 프로세스에서 `android_main`이 다시 불릴 수 있다. Rocks 정적 상태는 다시 들어오는 것을 전제로 하지 않으므로, `android_main`이 돌아오기 전에 `platform::ExitProcessWithoutTeardown(exitCode)`로 프로세스를 끝낸다(AD8).
## 4. 창과 렌더 surface

`platform::Window`의 Android 구현은 "앱의 그 창 하나"다. `Window::Create`는 앱이 하나만 만들 수 있고, `WindowDesc`의 크기·제목은 무시한다(전체 화면). 제목은 최근 앱 목록 이름이며 `SetTitle`은 Unsupported다.

| 앱 명령 | 처리 |
|---|---|
| `APP_CMD_INIT_WINDOW` | `ANativeWindow`를 잡고(`ANativeWindow_acquire`) 크기를 갱신한다. `NativeHandle().window`가 그 포인터가 되고 "창 있음" 세대가 올라간다 |
| `APP_CMD_TERM_WINDOW` | 창 파괴 gate(아래) 뒤 `ANativeWindow_release`, "창 없음" |
| `APP_CMD_WINDOW_RESIZED`·`CONFIG_CHANGED`(회전) | 크기 갱신, `ConsumeResize`가 알린다. 회전·크기 변경에 액티비티가 다시 만들어지지 않게 manifest 틀에 `android:configChanges`(orientation·screenSize·screenLayout·keyboardHidden 등, UE `UEDeployAndroid.cs:2833`)를 둔다 |
| `APP_CMD_GAINED_FOCUS`·`LOST_FOCUS` | `HasFocus`. 잃으면 `FocusLost`(모든 손가락 취소 — TouchInput 3.3절) |
| `APP_CMD_PAUSE`·`RESUME` | 앱 수명 `EnteredBackground`·`EnteredForeground` |
| `APP_CMD_LOW_MEMORY` | 앱 수명 `LowMemory`(PlatformLayer 이벤트 목록에 더함 — 이 개정에서 고쳤다) |
| `APP_CMD_DESTROY` | `WillTerminate` |

- **창 파괴 gate(AD3):** OS는 TERM_WINDOW 처리가 끝나면 surface를 없앤다. 그 전에 렌더가 그 surface를 놓아야 한다.
  1. 이벤트 스레드가 창 큐에 `SurfaceLost` 요청을 넣는다.
  2. 이벤트 스레드는 gate(`platform::ManualResetEvent`)를 기다린다.
  3. 게임 스레드는 다음 `PumpEvents`에서 이를 보고 호스트에 알린다. 호스트는 렌더에 swapchain·EGL window surface를 놓게 하고, 진행 중인 GPU 작업이 그 surface를 쓰지 않게 한다.
  4. 게임 스레드가 `Window::AcknowledgeSurfaceLost()`로 gate를 연다.
  5. 이벤트 스레드가 그 뒤 `ANativeWindow_release`를 하고 TERM_WINDOW를 끝낸다.
  - 기다리는 동안 Java UI 스레드도 막혀 있다(glue의 `android_app_set_window`는 `onAppCmd`가 끝날 때까지 기다린다). 그래서 UE 옛 창 동작처럼 제한 시간을 둔다: `SurfaceReleaseTimeoutMilliseconds`(= 4000, UE `EMDoneTrigger->Wait(4000)` — `LaunchAndroid.cpp:1654–1667`).
    - 넘으면 Error 로그를 남기고 진행한다. 렌더는 surface 상실 오류(`VK_ERROR_SURFACE_LOST_KHR`, `EGL_BAD_SURFACE`)를 정상 경로로 처리해야 한다.
    - 게임 스레드가 이미 끝났거나 끝나는 중이면 gate를 바로 연다.
    - 제한 시간은 ANR 방지 장치다. 시험은 이 시간에 기대지 않고, 명시 gate와 0 제한 시간 주입으로 두 경로를 결정적으로 확인한다.
  - UE 새 창 동작은 이벤트 스레드에서 RHI release 콜백을 바로 부른다(`:1567–1585`, `:1757–1764`). Rocks는 렌더 자원을 게임·렌더 쪽만 만지게 하려고 gate 방식을 쓴다.
- **surface 다시 만들기:** 창이 다시 생기면(INIT_WINDOW) `SurfaceAvailable`을 알리고 렌더가 swapchain을 다시 만든다. 창이 없는 동안 엔진은 돌되 present를 건너뛴다(`IsMinimized`가 true — 데스크톱 최소화와 같은 계약).
- **그래픽 연결:**
  - OpenGL ES: EGL 플랫폼 층이 `NativeHandle().window`로 window surface를 만든다. 창이 없으면 pbuffer다(OpenGLESGraphics 문서 그대로).
  - Vulkan: `VK_KHR_android_surface`로 만든다(VulkanGraphics 문서의 플랫폼 surface).

## 5. 입력 펌프

- 이벤트 스레드의 `onInputEvent` 콜백이 `AInputEvent`를 TouchInput 4.2절 규칙으로 `input::InputEvent`로 바꿔 창 큐에 넣는다. 터치 번호 배정기는 게임 스레드의 `EndInputFrame`이 프레임 경계를 알릴 때까지 끝난 번호를 잡아 둔다.
- 처리한 이벤트는 1, 처리하지 않은 것은 0을 돌려준다. 볼륨 키는 0이어서 OS가 처리한다. Back은 1(소비 — TouchInput 4.2절)이다.
- `Window::PumpEvents`(게임 스레드)는 데스크톱과 같은 모양으로 큐를 호스트 `InputQueue`로 옮긴다. 넘쳐서 버린 수는 손실로 센다.
- 게임패드는 `GamepadBackend::Poll`이 이벤트 스레드가 모은 패드 상태를 읽는다. `AINPUT_SOURCE_GAMEPAD`·`JOYSTICK`의 키·축은 UE `AndroidGameControllerMapper` 표준 배치를 따른다.

## 6. 로그·경로·명령줄

- **로그:** Android에는 stderr를 보는 사람이 없다. LogHost가 Android에서 기본 sink로 logcat sink(`__android_log_write`, tag는 앱 이름, Rocks 수준 → `ANDROID_LOG_*`)를 단다.
  - 한 줄 길이 제한(약 4 KB)을 넘으면 나눠 쓴다(이름 있는 상수).
  - 시험 실행 파일(adb shell)은 지금처럼 stderr다.
- **경로:** 3절의 `RegisterApplicationDirectories`가 `UserData`·`UserCache`를 정한다(PlatformLayer에 이미 있는 계약).
- **명령줄:** Intent extra `rocks.args`를 UTF-8 인자 배열로 받아 Launch의 명령줄 캡처에 넣는다. 개발 실행은 `adb shell am start -n <package>/.RocksActivity --esa rocks.args "--engine,--window"` 꼴이다.

## 7. APK 패키징 (Rocks.Build)

- **빌드 규칙:** 프로그램 규칙에 `AndroidPackaging = Apk`와 `AndroidPackageName`(예: `com.rocks.sandbox`)을 둔다.
- **패키징 단계(`package-android`):**
  1. `Build/Android/Template`의 Gradle 프로젝트 틀(Java 액티비티·manifest·`build.gradle`)을 `out/.../android/<Program>`에 만든다. manifest의 `minSdk`는 Toolchain의 Android API 값과 같다.
  2. 요청한 ABI마다 링크한 `lib<Program>.so`를 `jniLibs/<abi>`에 둔다. 기본은 arm64-v8a(기기)와 x86_64(에뮬레이터) 둘이다. libc++는 정적 링크라 공유 라이브러리를 더하지 않는다. Android 15의 16 KB 페이지 기기를 위해 링크에 `-Wl,-z,max-page-size=16384`를 준다.
  3. 쿠킹 데이터가 있으면 `assets/`에 낱개 파일로 둔다. 비압축으로 저장해 fd 읽기가 되게 한다(`noCompress`). UE는 `main.obb.png` 하나로 묶어 넣고 압축을 피하려고 확장자를 바꾼다(`UEDeployAndroid.cs:4906`). Rocks는 매니페스트가 이미 파일 단위라 낱개로 둔다(AD4).
  4. Gradle wrapper로 `assembleDebug`·`assembleRelease`를 돌린다. Debug는 SDK 기본 debug keystore로, Release는 설정의 keystore로 서명한다.
- **도구:** JDK 17, Android SDK(platform·build-tools), Gradle wrapper. 없으면 설치 스크립트가 받는다(`Build/Setup` — "필요한 도구는 설치" 원칙). NDK는 지금 선택 스크립트 그대로다.
- **설치·실행:** `Rocks.proj -t:Run -p:RocksPlatform=android -p:RocksTarget=Sandbox`가 `adb install -r`, `am start`, logcat 필터 출력을 한다. 에뮬레이터는 x86_64 AVD다.
- **APK 안 파일(VFS):** `platform::FileSystem`의 Android APK backend가 `AAssetManager`로 읽기 전용 열기·크기·읽기를 한다. 비압축 항목은 `AAsset_openFileDescriptor64` fd·오프셋으로 비동기 읽기를 한다(UE와 같다). 구현은 AssetSystem "VFS(APK)" 후속 항목이 맡고, 이 문서는 계약만 정한다.

## 8. 검증

이 PC에서 NDK·에뮬레이터로 할 수 있다.

| 검증 | 방법 |
|---|---|
| 단위 시험 | 앱 명령 → 상태 전이, 창 파괴 gate(게임 스레드가 늦게 확인해도 순서가 지켜지고, 게임 스레드가 끝났으면 바로 열림), 입력 변환, logcat 줄 나누기를 Android 없이 도는 순수 함수와 가짜 glue로 시험한다(EngineTests, Windows에서도 돈다). 순서는 명시 gate로 정하고 시간에 기대지 않는다 |
| 기기 단위 시험 | 기존처럼 `-t:Test -p:RocksPlatform=android`(adb 실행 파일)로 Android 전용 변환·경로 시험 |
| APK 실행 | Sandbox APK를 에뮬레이터에 설치·실행한다. logcat에서 엔진 시작·"Opened world Main"·종료 줄을 확인한다. 홈 버튼 → 복귀(TERM/INIT_WINDOW) 뒤에도 계속 도는지 확인한다 |
| 사람 확인 | 에뮬레이터·기기에서 터치·멀티터치·회전·Back·백그라운드 복귀 |

## 9. 작업 순서

| 단계 | 내용 | 선행 |
|---|---|---|
| AP1 | Launch Android 진입(`android_main`·이벤트 스레드·종료), logcat sink, 명령줄·경로 | 없음 |
| AP2 | PlatformWindow Android 창·앱 명령·창 파괴 gate·수명 이벤트 큐 연결 | AP1 |
| AP3 | APK 패키징(`package-android`·Gradle 틀·서명·`Run`), 도구 설치 | AP1 |
| AP4 | Sandbox APK(Android용 Sandbox 구성: GLES 또는 Vulkan만 링크), 에뮬레이터 실행 확인 | AP2, AP3, P11·P9 Android 실행 |
| AP5 | 입력(TouchInput TI5)·게임패드 | AP2, TouchInput TI1 |
| AP6 | APK VFS(AssetSystem 후속) | AP3 |

ShaderSystem P13이 막는 SH6(Android 셰이더·그래픽 실행)은 AP4까지 끝나면 시작할 수 있다.

## 10. 결정

| ID | 결정 | 근거 |
|---|---|---|
| AD1 | Java는 `NativeActivity` 상속 액티비티 하나, 경로·인자·오디오 포커스만 | UE 5.8과 같은 진입. Java 쪽 로직을 최소로 둔다. AndroidX games-activity는 쓰지 않는다(TouchInput TD7) |
| AD2 | 이벤트 스레드(glue)와 게임 스레드(새 스레드, 8 MB 스택)를 나눈다 | UE처럼 둘을 나눠 게임 프레임이 길어도 OS 콜백이 막히지 않게 한다. UE와 반대로 glue 스레드를 이벤트 쪽에 두면 looper를 옮길 필요가 없고 게임 스레드 스택을 정할 수 있다 |
| AD3 | 창 파괴는 게임 스레드 확인 gate 뒤에 놓고, 제한 시간(4초) 뒤에는 진행한다 | OS가 TERM_WINDOW 처리 뒤 surface를 없애므로 렌더가 먼저 놓아야 한다. UE 옛 창 동작도 4초 제한으로 기다린다. 무제한 대기는 UI 스레드를 막아 ANR이 된다 |
| AD4 | 쿠킹 데이터는 APK 안, 낱개 파일, 비압축 | APK 안에 두는 것은 UE `bPackageDataInsideApk`와 같다. 묶음 파일(UE `main.obb.png`) 대신 낱개인 것은 쿠킹 매니페스트가 파일 단위이기 때문이다. OBB·AAB는 배포 단계 |
| AD5 | 콘솔 시험 실행 파일은 `main`을 유지하고, APK 프로그램만 `android_main` | 지금의 adb 시험 경로를 깨지 않는다 |
| AD6 | Android 기본 로그 sink는 logcat | stderr를 보는 사람이 없다. UE도 logcat으로 낸다 |
| AD7 | 명령줄 extra는 debuggable 빌드에서만 받는다 | Release에서 받으면 외부 앱이 명령줄을 주입할 수 있다 |
| AD8 | `android_main` 끝에 프로세스를 끝낸다 | 같은 프로세스에서 `android_main`이 다시 불릴 수 있는데 Rocks 정적 상태는 재진입을 전제로 하지 않는다 |
| AD9 | `Window`의 Android 계약 변경(크기 무시 `Create`·세대가 바뀌는 `NativeHandle`·`AcknowledgeSurfaceLost`·surface 알림)은 AP2에서 `Window.h`에 적는다 | 데스크톱 계약(`Window.h:46–47` — 창보다 오래 보관하지 않는 고정 핸들)과 달라지는 부분을 공개 헤더에 명시해야 한다 |
