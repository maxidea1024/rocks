# 시작 화면(스플래시) — 10. 설정과 명령줄

[목차](README.ko.md)

## 10. 설정과 명령줄

### 10.1 `SplashSettings`(branch `Engine`, 섹션 `Splash`)

- 설정 struct `SplashSettings`는 **`Engine` 모듈에 정의하고 등록한다**(모든 대상에 있다). ClientOnly `SplashScreen`에 두면 Server 대상에 struct가 없어, 같은 프로젝트의 `Config/Engine.json`에 쓴 `Splash` 섹션이 서버에서 `UnknownSection`이 된다(Config 3.2절). struct는 값만 갖고, 읽는 쪽은 호스트와 `SplashScreen`이다. UE도 프로젝트 설정 클래스는 모든 대상에 있다.
- 기본값은 코드에만 둔다. 엔진 `Engine/Config/Engine.json`에는 섹션을 쓰지 않는다. 프로젝트는 자기 `Config/Engine.json`에 쓴다.
- 등록은 모듈 설정 등록 시점(층 파일을 읽기 전 — Config 5.4절 1단계, ModuleLoader 설정 등록)이다.

```json5
// <Project>/Config/Engine.json
"Splash": {
  "enabled": true,
  "game": {
    "image": "",              // 비면 5.2절 찾기
    "showTitle": true,
    "title": "",              // 비면 프로젝트 이름
    "showProgressText": false,
    "showProgressBar": true,
    "copyright": "",
    "layout": {}              // 6.4절 덮어쓰기(위치·크기·색)
  },
  "studio": { "image": "", "copyright": "", "layout": {} }
}
```

- `enabled`에는 사용자 층 쓰기를 허용하는 지정자를 단다(Config 3.1절 사용자 층). 플레이어가 사용자 설정 파일로 끌 수 있다. 사용자 층이 없는 봉인 모드(Sandbox)는 명령줄로만 끈다.
- 칸을 보일지는 `show*`만 정한다. `layout` 덮어쓰기에는 보이기 칸이 없다. Studio 칸 표시는 6.3절로 고정하고(엔진 도구의 일관성) `studio.layout`은 위치·크기·색만 바꾼다. `enabled`는 둘 다에 적용한다.
- 읽는 때: `CompleteStartup` 뒤라 봉인 값을 읽는다. 실행 중 바꿔도 반영하지 않는다(StartupOnly).

### 10.2 명령줄

| 옵션 | 효과 | 출시 빌드 | UE |
|---|---|---|---|
| `--no-splash` | 4절 `DisabledByCommandLine` | 받는다 | `-NOSPLASH` |

- `HostCommandLineOptions`(AppInfoCommandLine 10.2절)에 더한다.
- 이미지 경로 옵션은 두지 않는다. 개발 중에는 `--set`(세션 층)으로 `Splash.game.image`를 바꿀 수 있지만, 출시 빌드의 세션 층은 서버 대상만 받으므로(Config 3.1절) 출시 게임의 이미지는 프로젝트 설정으로만 정한다.

### 10.3 부트 순서에서의 자리

AppInfoCommandLine 8.3절 순서의 9단계(LogHost 설치·시작 로그) 뒤, 10단계(trace·엔진 루프) 전이다: `ProgressHub::Initialize` → `DecideSplashVisibility` → `StartupProgress::Begin` → `SplashScreen::Show`(Windows는 바로 돌아온다) → `Enter(HostServices)` → `StartPhase(PostConfigInit)` 이후 계속. UE와 같이 설정·지역화를 읽은 뒤이고 엔진 모듈 시작 전이다. 이 순서는 AppInfoCommandLine 8.3절 표에 더한다(18절).
