# 시작 화면(스플래시) — 8. 시작 단계 가중치와 보정

[목차](README.ko.md)

## 8. 시작 단계 가중치와 보정

### 8.1 단계

`StartupProgress`의 가장 바깥 구간은 아래 단계다. 단계는 enum `StartupStep`이고 호스트가 `Enter(step)`으로 넘긴다. 호스트가 밖에서 경계를 정할 수 있는 단위만 단계로 둔다. 한 단계는 한 번만 들어가고, 들어가지 않은 단계는 가중치 0으로 건너뛴다(그 호스트에 없는 일). 순서는 `Enter` 순서다.

| 단계 | 하는 일 | 안쪽 진행 | 기본 가중치(SP2 측정으로 바꿈) |
|---|---|---|---|
| `HostServices` | `StartPhase(PostConfigInit)`, trace 세션·콘솔 명령 | `StartPhase`의 모듈 수 구간 | 10 |
| `Engine` | `Engine::Initialize` 전체: 엔진 단계 모듈(PreDefault–PostDefault), Registry, EngineSubsystem, `PostEngineInit` | `Engine`이 여는 구간(아래) | 40 |
| `Assets` | 에셋 마운트·색인·너겟 준비(엔진 밖에서 하는 부분) | 구간·단계 계수 | 20 |
| `Shaders` | 셰이더 라이브러리·전역 셰이더(Studio는 필요 시 컴파일) | 단계 계수(job 수) | 15 |
| `Content` | 게임: GameInstance·기본 맵 열기. Studio: 에디터 모듈·UI·기본 레벨 | 구간 | 10 |
| `FirstFrame` | 주 창 첫 프레임 렌더·표시(9.4절) | 없음 | 5 |

- **`Engine` 안쪽.** `Engine` 모듈은 Server 대상에도 들어가므로 `SplashScreen`·`StartupProgress`를 모른다. Progress 모듈의 `ProgressScope`만 연다: 엔진 단계 모듈 60, Registry·EngineSubsystem 30, `PostEngineInit` 10(기본값). `ModuleLoader::StartPhase`는 모듈 수 구간과 문구 "Starting module <이름>"(Invariant Text)을 연다. 바인딩이 없는 실행(도구·시험·서버)에서는 구간이 아무 일도 하지 않는다(ProgressReporting 6.2절).
- 엔진 밖에서 에셋·셰이더를 준비하는 호스트가 아니면(지금의 Sandbox) `Assets`·`Shaders`는 들어가지 않는다.
- UE `EditorInit`의 50/40/10과 같은 역할이다. 설정·로그(스플래시 전)는 단계에 넣지 않는다.

### 8.2 측정 보정(SP5)

- UE는 고정 가중치라 기기와 프로젝트에 따라 한 단계에서 오래 멈춘다. Rocks는 지난 실행의 실제 시간을 가중치로 쓴다.
- 시계는 `StartupProgressDesc::nowNanoseconds`다(비면 ProgressHub 기본과 같은 단조 시계). 시험은 가짜 시계를 넣는다.
- 시작이 정상으로 끝나면(`Finish` — 첫 프레임 표시 뒤) 단계별 걸린 시간을 `StartupProgressDesc::profilePath`(호스트가 `{SavedDir}/Startup/StartupProfile.json`을 넘긴다, 비면 보정 끔)에 쓴다: `{"v":1, "<대상 이름>": {"HostServices": 0.12, "Engine": 0.84, …, "samples": 3}}`. 다음 실행은 저장값을 지수 평균(`kStartupProfileBlend` = 0.5 — 새 값 반)으로 갱신하고 가중치로 쓴다. 들어가지 않은 단계는 0, 저장값이 없는 단계는 코드 기본값의 비율로 채운다.
- 실패·취소·`--no-splash`·무인 실행은 쓰지 않는다(무인 실행은 캐시가 차가운 경우가 많아 값을 흐린다). 파일이 깨졌거나 `v`가 다르면 Warning 한 번 뒤 코드 기본값으로 시작하고 덮어쓴다. 쓰기는 임시 파일 뒤 이름 바꾸기다.
- 단계 하나의 가중치는 합의 `kStartupStepWeightFloor`(2%) 아래로 내려가지 않는다(그 단계에서 막대가 멈춰 보이지 않게).
- 모듈 단위·작업 단위 보정은 하지 않는다(단계 안쪽은 구간·계수 비율 그대로).
