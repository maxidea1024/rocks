# 시작 화면(스플래시) — 10. 설정과 명령줄

[목차](README.ko.md)

## 10. 설정과 명령줄

### 10.1 `ui.splash.*` 부팅 CVar(Engine 모듈)

- 설정은 설정 struct가 아니라 **부팅 CVar**다(개정 2). `Engine` 모듈(모든 대상에 있다)이 `RegisterSettings`에서 등록한다(`Engine/SplashSettings.h`의 `RegisterSplashSettings`). 오디오의 `audio.*`와 같은 방식이다.
  - 설정 struct(섹션 `Splash`)로 두면 struct의 리플렉션 타입을 Engine을 싣는 모든 호스트(Commandlet·시험·Sandbox)가 설정 문맥 타입 등록에 넣어야 하고(빠지면 모듈 설정 등록이 NotFound — Collision·Physics처럼), Engine 모듈의 첫 설정 struct라 여러 호스트를 함께 고쳐야 한다. CVar는 리플렉션 타입이 필요 없다.
  - ClientOnly `SplashScreen`에 두지 않는 이유는 그대로다: 서버 대상에도 이름이 있어야 같은 프로젝트 설정 파일을 서버가 읽는다.
- 이름 영역은 기본 표의 `ui`다(Config 5.1절 — 새 영역을 등록하지 않는다). 이름 문법상 단어는 `_`로 잇는다.

| 이름 | 종류 | 기본 | 뜻 |
|---|---|---|---|
| `ui.splash.enabled` | bool, `UserSetting` | true | 띄울지(4절 `DisabledBySettings`). 사용자 층에 쓸 수 있다 — 플레이어가 끈다 |
| `ui.splash.game.image` | String | "" | 게임 이미지 경로({ProjectDir}·{EngineDir} 펼침). 비면 5.2절 찾기 |
| `ui.splash.game.show_title` | bool | true | 게임 제목 칸 |
| `ui.splash.game.title` | String | "" | 제목 글자. 비면 프로젝트 이름(Sandbox는 "Rocks Sandbox") |
| `ui.splash.game.show_progress_text` | bool | false | 게임 진행 문구 칸(6.3절) |
| `ui.splash.game.show_progress_bar` | bool | true | 게임 진행 막대 |
| `ui.splash.game.copyright` | String | "" | 게임 저작권 칸 글자. 비면 숨김 |
| `ui.splash.studio.image` | String | "" | Studio 이미지 경로 |
| `ui.splash.studio.copyright` | String | "" | Studio 저작권 칸 글자 |

```json5
// <Project>/Config/Engine.json
"Settings": {
  "ui.splash.game.show_progress_text": true,
  "ui.splash.game.copyright": "(c) 2026 My Studio"
}
```

- 호스트가 부트에서 `ReadSplashSettings()`로 읽어 `SplashShowDesc`에 복사해 넘긴다. `SplashScreen` 모듈은 Config를 모른다.
- 부팅 값이므로 `CompleteStartup` 뒤 값이 고정되고 실행 중 바꿔도 반영하지 않는다.
- Studio 칸 표시는 6.3절로 고정한다(엔진 도구의 일관성). `ui.splash.enabled`는 둘 다에 적용한다.
- 사용자 층이 없는 봉인 모드(Sandbox)는 명령줄로만 끈다.

### 10.2 명령줄

| 옵션 | 효과 | 출시 빌드 | UE |
|---|---|---|---|
| `--no-splash` | 4절 `DisabledByCommandLine` | 받는다 | `-NOSPLASH` |

- 호스트 공용 옵션 `HostCommandLineOptions`(AppInfoCommandLine 10.2절)에 더한다. 그 전에는 호스트가 자기 파서로 받는다(Sandbox `--no-splash` — SP1).
- 이미지 경로 옵션은 두지 않는다. 개발 중에는 `--set=ui.splash.game.image=…`(세션 층)으로 바꿀 수 있지만, 출시 빌드의 세션 층은 서버 대상만 받으므로(Config 3.1절) 출시 게임의 이미지는 프로젝트 설정으로만 정한다.

### 10.3 부트 순서에서의 자리

AppInfoCommandLine 8.3절 순서의 9단계(LogHost 설치·시작 로그) 뒤, 10단계(trace·엔진 루프) 전이다: `ProgressHub::Initialize`(SP2) → `DecideSplashVisibility` → `StartupProgress::Begin`(SP2) → `SplashScreen::Show`(Windows는 바로 돌아온다) → `Enter(HostServices)`(SP2) → `StartPhase(PostConfigInit)` 이후 계속. UE와 같이 설정·지역화를 읽은 뒤이고 엔진 모듈 시작 전이다. 이 순서는 AppInfoCommandLine 8.3절 표에 더한다(18절). Sandbox는 SP1에서 이 자리(LogHost 뒤, trace 전)에 띄운다.
