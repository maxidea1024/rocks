# 시작 화면(스플래시) — 1. UE 5.8 조사

[목차](README.ko.md)

## 1. UE 5.8 조사

### 1.1 구조

- 플랫폼 정적 클래스 `FPlatformSplash`(`GPS`의 `FGenericPlatformSplash`를 플랫폼이 typedef)가 `Show`·`Hide`·`IsShown`·`SetSplashText(type, FText)`·`SetProgress(percent)`·`SetSplashImage`·`SetCustomSplashImage`·`RegisterUserSplashText/Image`를 준다. 기본 구현은 아무 일도 하지 않는다(콘솔·모바일).
- 글자 칸은 `SplashTextType`(`StartupProgress`·`VersionInfo1`·`CopyrightInfo`·`GameName`·`User1–6`), 이미지 칸은 `SplashImageType`(`Background`·`User1–6`)이다. 칸마다 글자 크기(`Small`·`Normal`·`Title`)·밝기(`Low`·`Medium`·`High`)·`WordWrap` 플래그가 있다. `User` 칸은 Show 전에만 등록한다(`WPS` 831–852행 `check(!GSplashScreenThread)`).

### 1.2 표시·숨김 지점

- **표시.** `FEngineLoop::PreInitPreStartupScreen`에서 엔진 지역화를 기다린 뒤(`LEL` 3105행) Slate 앱을 만들 때, 멀티스레드를 지원하고 `-RenderOffScreen`이 아니면 `Show`한다(`LEL` 3107–3115행). 그 앞에 high DPI를 켠다(`LEL` 3090행 `InitHighDPI` — "splash screen보다 먼저"). 설정·명령줄·pak은 이미 읽힌 뒤다.
- **게임 숨김.** `UGameEngine::Tick`의 첫 번째 호출이 `Hide` 뒤 게임 창을 보인다(`GE` 2009–2023행). 영상 플레이어 초기화(`DefaultGameMoviePlayer.cpp` 194행)와 셰이더 라이브러리 오류 대화상자 직전(`ShaderCodeLibrary.cpp` 2295행)도 `Hide`한다.
- **에디터 숨김.** `EditorInit`이 `FScopedSlowTask(100)`을 열고 50(`EngineLoop.Init`)·40(에디터 객체·`FUnrealEdMisc::OnInit`)·10(디렉터리·대상)으로 나눈 뒤 끝에 `Hide`한다(`UEG` 240–306행). 자동 저장 복구 대화상자 앞뒤로 `Hide`·`Show`한다(`UEM` 371–378행).
- **모듈 단계.** `PostSplashScreen` 단계 모듈을 Slate 렌더러 준비 뒤 올리고, `PreLoadScreen`의 `CustomSplashScreen`을 재생한다(`LEL` 3364–3387행). OS 스플래시와는 다른 Slate 기반 화면이다.

### 1.3 이미지 찾기(`GPSC` 63–115행)

1. `SetCustomSplashImage`로 준 경로가 있으면 그것만 본다.
2. 인자가 이미 있는 파일 경로면 그대로 쓴다.
3. 프로젝트 `Content/Splash/<이름>`, 다음 엔진 `Content/Splash/<이름>`. 확장자는 `.png`→`.jpg` 순서로 찾고 없으면 `.bmp`로 가정한다(`GPSC` 12–41행). 프로젝트에서 찾으면 "사용자 정의(custom)"다.
- 이름은 에디터 `EdSplash`, 게임 `Splash`(`WPS` 710행). 프로젝트 설정 화면(Windows 대상 설정의 Splash 범주)이 고른 그림을 프로젝트 `Content/Splash/{EdSplash,Splash}.bmp`로 복사한다(`Editor/DetailCustomizations/Private/WindowsTargetSettingsDetails.cpp` 47–50·217–289행).
- 패키징은 `Content/Splash`를 낱개 파일로 싣는다(`Config/BaseGame.ini` 283행 `+EngineExtraStageFiles=(Path="Content/Splash")`). 스플래시는 pak을 쓰기 전에도 떠야 하기 때문이다.

### 1.4 Windows 구현(`WPS`)

- `Show`는 `-NOSPLASH`가 없고 CVar `WindowsApplication.ShowSplash`(기본 true, 47–51행)가 켜져 있을 때 전용 스레드(스택 128 KiB, 783행)를 만든다. 스레드가 WIC로 이미지를 32bpp BGR로 풀고(alpha 무시, 243–438행) `WS_POPUP` 창을 주 모니터 가운데에 이미지 크기 그대로 만든다(486–510행). DPI 배율은 보지 않는다. Windows 11 둥근 모서리를 켠다(512–516행).
- 에디터 창은 작업 표시줄에 보이고(`WS_EX_APPWINDOW`), 게임은 보이지 않는다(`WS_EX_TOOLWINDOW`, 491–492행). 에디터는 끌어서 옮길 수 있다(43–45·217–228행).
- 글자는 GDI `DEFAULT_GUI_FONT`(11·12·18/28 px)와 8방향 검은 외곽선으로 그린다(110–206행). 칸 위치는 이미지 아래 가장자리 기준 고정 오프셋이다(583–625행).
- 메시지 루프는 `PeekMessage` 뒤 `Sleep(1/60 s)`를 반복한다(635–672행).
- `SetSplashText`는 **에디터이거나 저작권 칸일 때만** 반영한다(868–903행, 주석: "진행 정보는 최종 사용자에게 흥미롭지 않고 지역화도 잘 안 돼 있다"). 게임 스플래시에는 진행 문구가 없다. 게임은 사용자 정의가 아닌(엔진) 이미지일 때만 Show에서 게임 이름을 넣는다(756–759행).
- `SetProgress`는 작업 표시줄 진행(`ITaskbarList3`)이다(854–866행).
- `Hide`는 창 생성 이벤트를 기다린 뒤 `WM_CLOSE`를 보내고 스레드를 join한다(792–823행 — 창이 만들어지기 전 `Hide`의 교착 방지). 스플래시가 닫힐 때 주 창이 뒤로 가지 않게 보이지 않는 guard 창을 둔다(760–778행).

### 1.5 macOS·Linux

- macOS(`MPS`)는 Cocoa 메인 스레드에서 테두리 없는 `NSWindow`를 만들고(`MainThreadCall`, 287–310행) 글자 갱신은 `dispatch_async(main)`이다. CVar는 보지 않고 `-NOSPLASH`만 본다(181행). 글자 갱신의 에디터 전용 제한(저작권 칸만 예외)은 Windows와 같다(344행). 진행은 메인 스레드 펌프(`PumpMessages`)에 기댄다.
- Linux(`LPS`)는 SDL3 창과 FreeType(엔진 Roboto 글꼴)로 그리고 디스플레이 배율로 크기를 키운다(494행). 전용 스레드 없이 갱신 호출 때 그린다.

### 1.6 에디터의 진행 표시(`FCE`)

- 에디터 `FFeedbackContextEditor`가 슬로 태스크 스택을 받아, 스플래시가 떠 있으면 `ProgressReported`에서 문구를 고른다: 스택 위에서 `Important` 범위의 문구 → 없으면 맨 위 문구(632–656행). 문구 끝에 0.2 s마다 도는 점 4개를 붙이고 `"{percent} - {message}"`로 만든 뒤 `SetProgress`·`SetSplashText(StartupProgress)`를 부른다(660–699행).
- 슬로 태스크가 게임 스레드 전용이라 진행은 게임 스레드가 `EnterProgressFrame`할 때만 오른다(ProgressReporting 1절).
- 에디터 시작 후 슬로 태스크 창을 띄울 수 없는 동안에는 스플래시를 대체 진행 창으로 띄운다(`IsFallbackSplashScreenAllowed`: 무인·전용 서버·`-RenderOffScreen`이면 안 함, 447–454·554–558행).

### 1.7 UE의 한계(Rocks가 고칠 것)

| UE | 문제 | Rocks |
|---|---|---|
| Windows 창을 이미지 크기 그대로 | 150–200% 배율 모니터에서 작게 보인다 | `@2x` 이미지와 모니터 배율(5.3절) |
| 게임은 진행 문구 금지, 진행률 표시 없음 | 게임 시작이 길어도 멈춘 것처럼 보인다 | 게임은 진행 막대 기본 켬, 문구는 설정(6.3절) |
| 고정 가중치(50/40/10) | 진행률이 튀고 한 곳에 오래 멈춘다 | 단계 가중치 + 기기별 측정 보정(8절) |
| 진행은 게임 스레드 펌프에 기댄다 | 게임 스레드가 막히면 문구가 멈춘다 | 스플래시 스레드가 진행 중계를 직접 당긴다(7.1절) |
| `Sleep(1/60 s)` 반복 | 아무 일 없어도 깨어난다 | 대기 함수에 갱신 간격 제한 시간(9.2절) |
| 대화상자 직전 `Hide`를 호출처마다 | 빠뜨리면 대화상자가 스플래시에 가린다 | 플랫폼 대화상자 hook 하나(9.5절) |
| 긴 문구를 그냥 자름 | 칸을 넘친다 | 말줄임(6.4절) |
| alpha 무시 | — | 같음(배경색 위 합성, 비사각 창은 후속 — 17절) |
