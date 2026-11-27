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

**SP5 구현 결정(2026-11-27).**
- **준비 시점(`StartupProgress::MarkReady`).** 호스트가 실제 시작 일이 끝난 순간(주 창 첫 프레임을 낸 뒤 — 9.4절 1)에 부른다. 그때 지금 단계의 시간을 닫고 그 뒤(최소 표시 시간 대기·넘기기 프레임)는 재지 않는다. 대기는 일이 아니므로 프로필에 넣으면 다음 실행의 FirstFrame 몫이 부풀어 앞 단계 막대가 느려진다(8.3절). 부르지 않으면 `Finish`까지 잰다.
- **파일.** `StartupProgressDesc::profilePath`·`profileTarget`. 비면 보정을 끈다. 프로젝트 호스트는 `{SavedDir}/Startup/StartupProfile.json`, 프로젝트가 없는 엔진 프로그램(Sandbox)은 `{EngineDir}/Saved/Startup/StartupProfile.json`(UE 엔진 프로그램의 Saved와 같은 자리, git 무시 대상)을 넘긴다. 부모 폴더는 쓸 때 만든다.
- **형식.** `{"v":1,"<대상>":{"HostServices":0.004,...,"samples":3}}` — 단계 이름 키(`StartupStepName`), 값은 초(유한한 0 이상). 다른 대상의 항목은 쓸 때 그대로 옮긴다. 읽기 상한 `MaxStartupProfileBytes`(64 KiB).
- **읽기(`Begin`).** 깨진 JSON·`v` 불일치·형식 오류(대상이 객체가 아님, 단계 값이 수가 아니거나 0 미만·`MaxStartupStepSeconds`(600초) 초과, `samples`가 부호 없는 정수가 아님)는 Warning 한 번 뒤 코드 기본값이고 성공한 시작이 덮어쓴다. 파일이 유효하고 그 대상 항목만 깨졌으면 그 항목만 버린다(다른 대상 항목은 쓸 때 남는다). 파일 없음은 조용히 기본값. 없음이 아닌 읽기 IO 오류(잠금·권한, 프로필 자리가 폴더)는 Warning 뒤 이번 실행이 쓰지 않는다(다른 대상 항목을 모르는 채 덮어쓰지 않게). 대상 이름은 `v`일 수 없다(`InvalidArgument`).
- **가중치.** 밝힌 단계 가운데 저장값이 있는 단계는 그 초, 없는 단계는 코드 기본값 × (저장된 단계 초 합 ÷ 그 단계들의 코드 기본값 합)로 채운다. 저장된 초 합이 0이면 모두 코드 기본값. 그다음 각 단계를 합의 `StartupStepWeightFloor`(2%) 이상으로 올린다(올린 뒤 합으로 한 번 더 — 단계 수 × 2%는 1보다 훨씬 작다).
- **쓰기(`Finish(Succeeded)`만).** 들어간 단계: 저장값이 있으면 `0.5 × 저장 + 0.5 × 이번`(`StartupProfileBlend`), 없으면 이번 값. 밝히지 않았거나 들어가지 않은 단계는 쓰지 않는다(다음 실행이 코드 기본값 비율로 채운다). 이번 측정값은 `MaxStartupStepSeconds`로 자른다(디버거 중단 같은 이상값이 여러 실행에 걸쳐 막대를 왜곡하지 않게). `MarkReady` 뒤의 `Enter`는 오용 Warning 뒤 무시한다(0초 단계가 다음 실행 몫을 바닥으로 떨어뜨리지 않게). `samples`는 이전 + 1(상한에서 멈춤). 임시 파일(`<경로>.<pid>.tmp`)에 쓰고 Flush한 뒤 `FileSystem::Replace`. 한계: 같은 프로필을 쓰는 두 실행이 겹치면 마지막 쓰기만 남는다(Begin 때 읽은 원문으로 쓰므로). 다음 실행이 정상 파일을 읽으므로 따로 잠그지 않는다. 쓰기 실패는 Warning 뒤 계속한다(시작은 이미 끝났다). 호스트 스레드에서 `Finish` 안에서 한다(파일 하나, 수 KiB). 빌드 증거의 소스 목록(`Rocks.Tooling.Common/BuildEvidence.cs` `RunOutputDirectories`)은 `.gitignore`와 같은 고정 실행 산출물 폴더(`Saved`·`Intermediate`·`Engine/Saved`·`Engine/Intermediate`·`Programs/Sandbox/TestContent/Saved`·`…/Intermediate`)를 세지 않는다 — 같은 이름의 소스 폴더는 그대로 센다(ToolTests `SourceSnapshot.IgnoresRunOutputFoldersOnly`)(git 무시 대상인 실행 산출물 — 빌드 중에 엔진 프로그램이 돌며 너겟 캐시·이 프로필을 써도 "소스가 바뀌었다"로 빌드가 실패하지 않게, SP5에서 고침).

### 8.3 기본 가중치 측정(SP2, 2026-11-27)

`StartupProgress::Finish`가 단계 시간을 Info 로그(`Startup steps (s): …`)로 남긴다. Sandbox(Debug, `--engine --window`, Windows 11 개발 기기) 3회:

| 회 | HostServices | Engine | FirstFrame |
|---|---|---|---|
| 1(콜드) | 0.007 | 0.075 | 4.077 |
| 2 | 0.007 | 0.060 | 2.097 |
| 3 | 0.008 | 0.055 | 2.207 |

- FirstFrame은 대부분 실제 일이다(SP5에서 로그 시각으로 다시 봄, 바로잡음 — 처음에는 최소 표시 대기로 읽었다): D3D12 장치 생성 약 0.2초, 그다음 개발용 셰이더 공급자(`DevelopmentShaderProvider`, ShaderSystem 16.3절 감시 재컴파일과 함께 들어옴)가 엔진 셰이더를 준비하는 약 2.3초 뒤 첫 프레임이 나간다. 1회째 4초는 셰이더 캐시가 차가운 경우다. SP5의 `MarkReady`(첫 프레임 직후)로 그 뒤의 최소 표시 대기·넘기기 프레임은 빠진다(이 측정에서는 일이 2초보다 길어 대기가 없었다).
- 그래서 Sandbox 프로필은 HostServices·Engine이 합쳐 3–4%, FirstFrame이 96% 안팎이 된다(HostServices는 바닥 2%). FirstFrame에는 안쪽 진행이 없어 막대가 그 몫 안에서 멈춰 보인다. 후속: 엔진 셰이더 준비를 `Shaders` 단계(단계 계수 = 셰이더 수)로 보고하면 막대가 움직인다 — 렌더 세션의 PG5(셰이더 컴파일 관리자 진행 연결) 때 Sandbox가 `Shaders` 단계를 밝힌다.
- 결론: Sandbox에는 무거운 단계(Assets·Shaders·Content)가 없어 이 측정으로 게임·Studio 기본값을 정할 수 없다. 기본 가중치(8.1절 표)는 설계값 그대로 두고, 기기·대상별 보정은 SP5(8.2절)가 지난 실행 시간으로 한다. SP5는 FirstFrame에서 최소 표시 대기를 빼고 저장한다(대기는 일이 아니다 — 넣으면 다음 실행의 FirstFrame 몫이 부풀어 앞 단계 막대가 느려진다).
- 시간이 짧은 실행에서 막대는 앞 단계를 금방 지나 FirstFrame 몫 안에서 머문다. 진행 문구는 "Showing the first frame"이고 퍼센트는 99에서 막힌다(7.2절).

