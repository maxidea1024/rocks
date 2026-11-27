# 시작 화면(스플래시) — 8. 시작 단계 가중치와 보정

[목차](README.ko.md)

## 8. 시작 단계 가중치와 보정

### 8.1 단계

`StartupProgress`의 가장 바깥 구간은 아래 단계다. 단계는 enum `StartupStep`이고 호스트가 `Enter(step)`으로 넘긴다. 호스트가 밖에서 경계를 정할 수 있는 단위만 단계로 둔다. 한 단계는 한 번만 들어가고, 순서는 `Enter` 순서다. **호스트는 들어갈 단계를 `StartupProgressDesc::steps`로 미리 밝힌다**(SP2에서 정함): 가장 바깥 구간의 총량은 시작할 때 정해야 하므로, 들어가지 않을 단계를 빼야 막대가 끝에서 뛰지 않는다. 밝히지 않은 단계에 `Enter`하거나 두 번 들어가면 Warning 한 번 뒤 무시한다. 비면 모든 단계다.

| 단계 | 하는 일 | 안쪽 진행 | 기본 가중치(SP2 측정으로 바꿈) |
|---|---|---|---|
| `HostServices` | `StartPhase(PostConfigInit)`, trace 세션·콘솔 명령 | `StartPhase`의 모듈 수 구간 | 10 |
| `Engine` | `Engine::Initialize` 전체: 엔진 단계 모듈(PreDefault–PostDefault), Registry, EngineSubsystem, `PostEngineInit` | `Engine`이 여는 구간(아래) | 40 |
| `Assets` | 에셋 마운트·색인·너겟 준비(엔진 밖에서 하는 부분) | 구간·단계 계수 | 20 |
| `Shaders` | 셰이더 라이브러리·전역 셰이더(Studio는 필요 시 컴파일) | 단계 계수(job 수) | 15 |
| `Content` | 게임: GameInstance·기본 맵 열기. Studio: 에디터 모듈·UI·기본 레벨 | 구간 | 10 |
| `FirstFrame` | 주 창 첫 프레임 렌더·표시(9.4절) | 없음 | 5 |

- **`Engine` 안쪽.** `Engine` 모듈은 Server 대상에도 들어가므로 `SplashScreen`·`StartupProgress`를 모른다. Progress 모듈의 `ProgressScope`만 연다: 엔진 단계 모듈 60, Registry·EngineSubsystem 30, `PostEngineInit` 10(기본값). `ModuleLoader::StartPhase`의 모듈 수 구간과 문구 "Starting module <이름>"(Invariant Text)은 로더의 시작 관찰자에 건 Engine `ModuleStartProgress`가 연다([모듈 로더](../ModuleLoader/03-Design/03.16-StartObserver.ko.md) 3.16절 — 로더는 부트 묶음이라 Progress에 의존하지 않는다). `Engine::Initialize`가 엔진 묶음·`PostEngineInit` 둘레에, 호스트가 `PostConfigInit` 둘레에 건다. 문구 Text는 구간이 발행할 때(`ProgressScope::IsReporting`)만 만든다. 바인딩이 없는 실행(도구·시험·서버)에서는 구간이 아무 일도 하지 않는다(ProgressReporting 6.2절).
- 엔진 밖에서 에셋·셰이더를 준비하는 호스트가 아니면(지금의 Sandbox) `Assets`·`Shaders`는 들어가지 않는다.
- UE `EditorInit`의 50/40/10과 같은 역할이다. 설정·로그(스플래시 전)는 단계에 넣지 않는다.

### 8.2 측정 보정(SP5)

- UE는 고정 가중치라 기기와 프로젝트에 따라 한 단계에서 오래 멈춘다. Rocks는 지난 실행의 실제 시간을 가중치로 쓴다.
- 시계는 `StartupProgressDesc::nowNanoseconds`다(비면 ProgressHub 기본과 같은 단조 시계). 시험은 가짜 시계를 넣는다.
- 시작이 정상으로 끝나면(`Finish` — 첫 프레임 표시 뒤) 단계별 걸린 시간을 `StartupProgressDesc::profilePath`(호스트가 `{SavedDir}/Startup/StartupProfile.json`을 넘긴다, 비면 보정 끔)에 쓴다: `{"v":1, "<대상 이름>": {"HostServices": 0.12, "Engine": 0.84, …, "samples": 3}}`. 다음 실행은 저장값을 지수 평균(`kStartupProfileBlend` = 0.5 — 새 값 반)으로 갱신하고 가중치로 쓴다. 들어가지 않은 단계는 0, 저장값이 없는 단계는 코드 기본값의 비율로 채운다.
- 실패·취소·`--no-splash`·무인 실행은 쓰지 않는다(무인 실행은 캐시가 차가운 경우가 많아 값을 흐린다). 파일이 깨졌거나 `v`가 다르면 Warning 한 번 뒤 코드 기본값으로 시작하고 덮어쓴다. 쓰기는 임시 파일 뒤 이름 바꾸기다.
- 단계 하나의 가중치는 합의 `kStartupStepWeightFloor`(2%) 아래로 내려가지 않는다(그 단계에서 막대가 멈춰 보이지 않게).
- 모듈 단위·작업 단위 보정은 하지 않는다(단계 안쪽은 구간·계수 비율 그대로).

### 8.3 기본 가중치 측정(SP2, 2026-11-27)

`StartupProgress::Finish`가 단계 시간을 Info 로그(`Startup steps (s): …`)로 남긴다. Sandbox(Debug, `--engine --window`, Windows 11 개발 기기) 3회:

| 회 | HostServices | Engine | FirstFrame |
|---|---|---|---|
| 1(콜드) | 0.007 | 0.075 | 4.077 |
| 2 | 0.007 | 0.060 | 2.097 |
| 3 | 0.008 | 0.055 | 2.207 |

- FirstFrame은 장치·렌더 lane 시작과 첫 프레임, 그리고 **최소 표시 시간(9.7절, 기본 2초) 대기**를 함께 담는다(넘기기는 `CanHide()` 뒤). 그래서 앞 단계가 70 ms 안에 끝나는 Sandbox에서는 FirstFrame이 거의 2초로 고정된다. 1회째는 장치 생성 등 콜드 비용이다.
- 결론: Sandbox에는 무거운 단계(Assets·Shaders·Content)가 없어 이 측정으로 게임·Studio 기본값을 정할 수 없다. 기본 가중치(8.1절 표)는 설계값 그대로 두고, 기기·대상별 보정은 SP5(8.2절)가 지난 실행 시간으로 한다. SP5는 FirstFrame에서 최소 표시 대기를 빼고 저장한다(대기는 일이 아니다 — 넣으면 다음 실행의 FirstFrame 몫이 부풀어 앞 단계 막대가 느려진다).
- 시간이 짧은 실행에서 막대는 앞 단계를 금방 지나 FirstFrame 몫 안에서 머문다. 진행 문구는 "Showing the first frame"이고 퍼센트는 99에서 막힌다(7.2절).

