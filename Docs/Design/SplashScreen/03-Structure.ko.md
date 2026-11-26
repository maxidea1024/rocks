# 시작 화면(스플래시) — 3. 구조 개요

[목차](README.ko.md)

## 3. 구조 개요

새 런타임 모듈 **`SplashScreen`**(`Engine/Source/Runtime/SplashScreen`)을 둔다. 모듈 type은 ClientOnly다(서버 대상에 들어가지 않는다 — `TextLayoutBackend`와 같다). 로딩 단계는 `None`이고 호스트가 명시해서 쓴다.

| 구성 | 하는 일 | 위치 | UE 대응 |
|---|---|---|---|
| `SplashSettings` | 프로젝트·사용자 설정(10절) | `Engine` 모듈 public, 리플렉션 struct(SPD13) | 프로젝트 설정 Splash 범주, CVar `WindowsApplication.ShowSplash` |
| `DecideSplashVisibility` | 띄울지 순수 판정(4절) | public | `Show`의 조건들 |
| `ResolveSplashImage` | 이미지 찾기·DPI 변형 고르기(5절) | public | `GetSplashPath` |
| `SplashLayout` | 글자 칸·막대 배치(6절) | public | 고정 오프셋 + `RegisterUserSplashText` |
| `SplashModel` | 화면에 보일 값(글자·진행·막대 보간)과 변경 표시. OS 없음, 주입 시계 | private, 시험 공개 | `GSplashScreenText` 등 전역 |
| `SplashPresenter` | OS 창·이미지 디코딩·글자 그리기·작업 표시줄 진행(Windows·macOS) | private 플랫폼 폴더 | `F*PlatformSplash` |
| `SplashScreen` | 공개 객체: Show·Hide·Suspend·Resume·SetText(11절) | public | `FPlatformSplash` |
| `StartupProgress` | 시작 작업·단계 가중치·측정 보정(7·8절) | public | `EditorInit`의 `FScopedSlowTask(100)` |
| `StartupSplashSource` | 진행 중계 스냅샷 → 스플래시 상태(7.2절) | private | `FFeedbackContextEditor::ProgressReported` |

- **의존.** `Core`·`PlatformBase`(`Thread`·대화상자 hook)·`Engine`(`SplashSettings` 값 — struct만 쓴다)·`Localization`(문구 해석)·`Progress`(ProgressReporting PG2)·`PlatformWindow`(데스크톱 세션 상태·macOS `RunOnMainThread`). Windows는 WIC·GDI·DWM·`ITaskbarList3`, macOS는 AppKit·ImageIO·CoreText. 외부 라이브러리는 없다. 이미지 디코딩에 `ImageCodec`(Developer)을 쓰지 않는 것은 출시 게임에 들어가야 하고 OS 디코더로 충분하기 때문이다(UE와 같다).
- **`PlatformWindow`의 `Window`를 쓰지 않는다.** `Window`는 한 스레드가 펌프하고 입력을 큐에 넣는 게임 창이다. 스플래시 창은 자기 스레드(Windows)나 Cocoa 메인 스레드(macOS)가 소유하는 단순 창이라 따로 만든다(UE도 `FGenericApplication` 창과 별개다).
- 모듈 로더에 UE `PostSplashScreen`·`PreEarlyLoadingScreen`·`PreLoadingScreen` 단계를 더하지 않는다. 그 단계들은 Slate 기반 사용자 정의 시작·로딩 화면용이고 OS 스플래시와 무관하다(ModuleLoader 9.1절 행을 이 문서로 갱신 — 18절).
