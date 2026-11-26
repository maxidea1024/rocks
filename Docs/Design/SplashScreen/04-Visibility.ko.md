# 시작 화면(스플래시) — 4. 표시 여부

[목차](README.ko.md)

## 4. 표시 여부

`DecideSplashVisibility(const SplashVisibilityInput&) → SplashSkipReason`은 부수 효과 없는 함수다. 호스트가 부르고 `None`이면 `SplashScreen::Show`를 부른다(Show는 판정하지 않는다). 위에서부터 처음 맞는 사유를 돌려준다.

| 순서 | 사유 | 조건 | UE |
|---|---|---|---|
| 1 | `UnsupportedPlatform` | Windows·macOS가 아님 | 기본 구현 no-op |
| 2 | `NotInteractiveHost` | `HostKind`가 Game이 아님(Commandlet·Tool·TestRunner) | Slate 앱이 아니면 안 띄움(`LEL` 3107행) |
| 3 | `NoMainWindow` | 이 실행이 주 창을 열지 않음(`opensMainWindow == false`) | 같은 조건(`bIsRegularClient`) |
| 4 | `DisabledByCommandLine` | `--no-splash` | `-NOSPLASH` |
| 5 | `Headless` | `RunTraits::headless` | `-nullrhi` 계열·`-RenderOffScreen` |
| 6 | `Unattended` | `RunTraits::unattended` | 대체 스플래시의 `IsUnattended` |
| 7 | `DisabledBySettings` | `SplashSettings::enabled == false`(프로젝트·사용자 층) | CVar `WindowsApplication.ShowSplash` |
| 8 | `NoDesktopSession` | `platform::QueryDesktopSessionState()`가 `Disconnected`(원격 연결을 끊었거나 사용자를 전환함). `Unknown`(조회 실패·macOS)은 띄운다 | — |
| 9 | `NoMainThreadHost` | macOS에서 `ApplicationSettings::mainThreadHost`가 없음(본문이 메인 스레드를 막는다 — 9.3절) | — |

호스트별 입력은 아래와 같다. `HostKind`에는 Studio가 따로 없으므로(AppInfo 8.1절 — 실행 종류이지 대상 종류가 아니다) Studio인지는 호스트가 `SplashKind`로 정한다.

| 실행 | `HostKind` | `opensMainWindow` | `SplashKind` |
|---|---|---|---|
| 게임(Game 대상, 패키지 포함) | Game | headless가 아니면 true | Game |
| Rocks Studio | Game | true | Studio |
| Sandbox `--engine --window` | Game | true | Game |
| Sandbox `--engine`(창 없음 — 자체 시험이 띄우는 `--frames N` 자식 포함) | Game | false | — |
| RocksCommandlet·도구 | Commandlet·Tool | false | — |
| 시험 실행기·probe | TestRunner | false | — |

- `HostKind`·`RunTraits`는 AppInfo([앱 정보와 명령줄](../AppInfoCommandLine.ko.md) 8절) 값이다. AppInfo 구현 전에는 호스트가 같은 뜻의 값을 채워 넘긴다(Sandbox는 자기 명령줄에서).
- Server 대상에는 `SplashScreen` 모듈이 없으므로 판정 자체를 하지 않는다.
- 판정 뒤 이미지 찾기 실패(`ImageNotFound`·`ImageInvalid`)와 창 생성 실패(`WindowFailed`)도 같은 enum으로 보고한다. 로그 수준은 1–3·9가 Debug, 4–8이 Info, 이미지·창 실패가 Warning이다(요구 6). 범주는 `LogSplash`(`"Splash"`).
- Studio의 Tryout을 별도 프로세스로 띄울 때(나중)는 자식에 `--no-splash`를 넘긴다.
