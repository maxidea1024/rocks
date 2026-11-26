# 시작 화면(스플래시) — 5. 이미지

[목차](README.ko.md)

## 5. 이미지

### 5.1 이름과 위치

| 종류 | 이름 | 엔진 기본(이 개정에서 넣음) |
|---|---|---|
| 게임·엔진 창 프로그램 | `Splash` | `Engine/Content/Splash/Splash.png`, `Splash@2x.png` |
| Studio | `StudioSplash` | `Engine/Content/Splash/StudioSplash.png`, `StudioSplash@2x.png` |

UE의 `EdSplash`는 Rocks 이름(Studio)에 맞춰 `StudioSplash`로 한다.

### 5.2 찾는 순서(`ResolveSplashImage`)

1. 설정 `SplashSettings::<종류>.image`가 비어 있지 않으면 그 경로만 본다(`{ProjectDir}`·`{EngineDir}` 펼침 — Config 경로 변수). 없으면 `ImageNotFound`(UE `SetCustomSplashImage`와 같다 — 다른 자리로 넘어가지 않는다).
2. 프로젝트 `{ProjectDir}/Content/Splash/<이름>`.
3. 엔진 `{EngineDir}/Content/Splash/<이름>`.
- 각 자리에서 확장자 `.png`→`.jpg`→`.bmp` 순서로 찾는다(UE와 같다 — `GPSC` 85·95행이 `.bmp` 경로도 존재를 확인한다). 스플래시는 에셋이 아니라 낱개 파일이므로 텍스처의 무손실 원본 규칙을 적용하지 않는다(UE와 같다).
- 프로젝트가 없는 프로그램(Sandbox — 설정 봉인 모드)은 3만 본다.
- 프로젝트 `Content/` 아래 PNG는 에셋 색인이 보지 않는다(AssetIndex는 `.rasset`만 — AssetSystem 7절).

### 5.3 DPI와 크기

- 1x 이미지의 픽셀 크기가 **논리 크기**다. `@2x`만 있으면 그 크기의 절반이고 가로·세로가 짝수여야 한다. 둘 다 있으면 `@2x`가 정확히 두 배여야 한다. 어기면 `ImageInvalid`.
- 창은 주 모니터 작업 영역 가운데에 `논리 크기 × 모니터 배율`(반올림)로 만든다. 배율이 1.0을 넘고 `@2x`가 있으면 `@2x`를, 아니면 1x를 원본으로 고른다(큰 원본을 고품질로 줄이는 쪽이 작은 원본을 키우는 것보다 선명하다). 원본과 창 크기가 다르면 고품질 축소·확대(Windows WIC Fant, macOS `CGContext` 고품질 보간)로 한 번 맞춘다.
- 창은 Show 때 주 모니터 배율로 정하고, 스플래시가 떠 있는 동안 모니터 배율이 바뀌어도 다시 만들지 않는다(Windows `WM_DPICHANGED`는 위치만 따른다 — 짧게 사는 창이라 단순하게).
- Windows 스플래시 스레드는 스레드 DPI 문맥을 per-monitor v2로 둔다(9.2절). 스플래시가 프로세스 DPI 모드를 바꾸지 않으므로 `--no-splash` 여부로 주 창 동작이 달라지지 않는다. UE는 스플래시 전에 프로세스 high DPI를 켠다(`LEL` 3090행) — Rocks에서 그 일은 PlatformWindow 작업이다(18절).

### 5.4 형식과 한도

- 디코딩: Windows WIC(PNG·JPEG·BMP), macOS ImageIO. 결과는 8-bit sRGB BGRA 하나로 바꾼다. alpha가 있으면 레이아웃의 배경색(기본 Rocks Black `#111111`) 위에 합성한다. 색 관리(ICC)는 하지 않는다.
- 한도: 파일 `kMaxSplashImageBytes`(32 MiB), 가로·세로 `kMaxSplashImageDimension`(4096 px), 논리 크기 최소 `kMinSplashLogicalDimension`(160 px). 넘거나 디코딩이 실패하면 `ImageInvalid`.
- 디코딩은 스플래시 스레드(Windows)·메인 스레드(macOS)에서 한다(호스트 스레드에 COM 아파트를 만들지 않는다).

### 5.5 배포

- 스플래시는 에셋 시스템이 서기 전에 떠야 하므로 쿠킹 패키지가 아니라 낱개 파일로 싣는다(UE `EngineExtraStageFiles`와 같다). 게임 패키지는 `{EngineDir}/Content/Splash`의 `Splash*` 파일(프로젝트가 자기 것을 가지면 생략)과 `{ProjectDir}/Content/Splash`를 싣고, `StudioSplash*`는 싣지 않는다. 패키징 단계 설계가 이 규칙을 가져간다(18절).
- 설치형 SDK(`InstalledSdk`)는 `Engine/Content/Splash`를 싣는다(SP1).
