## 1. 측정과 원인

### 1.1 자료와 방법

- 자료: 이 기계의 모든 작업 사본(`c:\work\rocks`, `.claude\worktrees\*` 34개, `%TEMP%\claude\**\wt-*`, `audio-wt`)의 `out/*/native-cache.json`·`action-report.json`·`build-evidence.json`·`compile_commands.json`·`*.obj.deps`(출력 폴더 301개), `%TEMP%\claude` 아래 빌드 로그의 `Native build: …; executed=…; cached=…` 줄, coordinator가 wt-ut5에서 남긴 `scratchpad/build-sb-probe.log`·`build-sb-probe2.log`.
- 단계 시간은 `build-evidence.json`의 `steps`(prerequisite·metadata·native, `durationNs`)다. 셰이더 파라미터 생성은 metadata 단계 뒤·native 단계 앞에 돌며(`Rocks.Build.Cli/Program.cs:356`) 따로 기록되지 않는다. 메타 모듈별 시간은 어디에도 기록되지 않는다 — 그래서 MI0(5.2절)에서 먼저 잰다. `--timing`은 native compile의 `-ftime-trace` 집계라 메타 단계를 재지 않는다.
- **대리 key**(proxy key): 저장된 action key는 해시라서 작업 사본 사이에 비교할 수 없다. action마다 모듈, 종류, 모듈 인자 서명(`compile_commands.json`의 인자에서 `/Fo`·소스 경로를 빼고 루트를 `<R>`, 출력 폴더를 `<O>`로 바꿈), 의존 기록(`*.obj.deps` — 컴파일러가 연 파일마다 내용 해시, 경로는 같게 바꿈), 소비 PCH의 대리 key로 루트와 무관한 key를 다시 만들었다. 의존 기록에는 소스·묶음 파일·정의 헤더·생성 헤더가 모두 있으므로 이 key가 같으면 컴파일러 입력이 같다.
- **헤더 이름 목록 조건**: 지금 native key는 include·소스 디렉터리의 헤더 이름 목록도 담는다(UnityBuildPch 4.1절). 작업 사본 사이 비교에는 두 commit의 `git ls-tree`로 그 목록이 같은지를 더 따졌다.
- 시간 가중은 마지막 실행에서 돈 action의 `DurationMilliseconds`(compile CPU 시간, 병렬 합)다. 측정 스크립트는 저장소 밖(세션 scratchpad)에 두었다.

### 1.2 빌드 시간의 구성

| 빌드 | native executed | metadata 단계(초) | native 단계(초) | 전체(초) |
|---|---|---|---|---|
| wt-ut5 Sandbox Debug(`DevelopmentShaderProvider.cpp` 하나 수정, coordinator 측정) | 9(선행 ShaderCompileWorker 0) | 대부분 | — | **523** |
| sparks EngineTests Debug(무변경) | 3 | 652 | 51 | 720 |
| ma2b EngineTests Debug(무변경) | 3 | 526 | 32 | 572 |
| audio-wt Sandbox Debug(무변경) | 3 | 392 | 12 | 429 |
| splash Sandbox Debug(무변경) | 0 | 287 | 16 | 321 |
| wt-ut5 Commandlet Debug(새 작업 사본) | 3,070 | 512 | 126 | 648 |
| wt-ut5 UIDemo Debug | 2,142 | 217 | 94 | 326 |
| wt-ut5 EngineModular Debug | — | 875 | 25 | 913 |
| material EngineTests Debug(origin 병합 중) | 3,426 | 713 | 214 | 954 |

- **메타 단계가 전체의 75–95%다.** native가 3개만 실행한 무변경 빌드도 5–12분 걸린다.
- wt-ut5 Sandbox 증분 빌드(523초)의 native는 `executed=9; cached=2680`, 선행 ShaderCompileWorker는 `executed=0; cached=53`이다. compile 증분은 잘 된다. 로그에는 리플렉션 모듈 25개 모두 `Rocks metadata: module X: N types; generated`, 안정 ID 검사 25회와 `Stable identity set (2948 IDs in 25 manifests): PASS`, 셰이더 파라미터 모듈 2개(`RenderBase`·`Sandbox`) `generated`가 나온다. coordinator 관찰로 바쁜 기계에서 모듈마다 15초 넘게, MaterialSchema(233 types)는 혼자 수 분 걸렸다.
- native 단계는 20 코어 병렬이다(Commandlet: compile CPU 902초가 벽시계 126초).
- compile CPU의 구성(Commandlet·UIDemo·EngineTests 순): 공개 헤더 검사 TU 41%·35%·48%, 서드파티 34%·37%·1%, 엔진·시험 소스가 나머지다.

### 1.3 처음부터 빌드하는 빈도

빌드 로그(최근 4일, 같은 실행의 로그·task 출력 중복 제거) 805회 중 107회가 `cached=0`이고 300개 넘게 실행한 빌드다. 이 107회가 4일간 실행한 native action 328,295개 중 182,370개(56%)다. 107회 중 100회가 `%TEMP%\claude\…\wt-*`의 새 작업 사본이다. 같은 기간 `cached>1000, executed≤5`인 빌드가 200회다.

### 1.4 원인 1: 메타 단계가 입력과 무관하게 매번 돈다

코드 근거(`origin/feature/engine-runtime` d2f2b9e8b 기준 줄 번호).

| 무엇 | 어디 | 매번 하는 일 |
|---|---|---|
| 메타 단계 진입 | `Rocks.Build.Cli/Program.cs:345` | `MetadataRunner.GenerateAsync`를 조건 없이 부른다 |
| stage | `MetadataRunner.cs:93` | 새 `<out>/.metadata-stage/<Guid>`. 이전 결과를 보지 않는다(클래스 주석 "이전 빌드의 게시본을 보지 않는다") |
| 모듈 실행 | `MetadataRunner.cs:122–224` | 도달하는 메타 모듈마다 **차례로**(의존 순서 `foreach`) `dotnet Rocks.MetaCompiler @<rsp>`(197행)를 띄운다. 건너뛰는 판정이 없다 |
| ID 검사 | `MetadataRunner.cs:227–236`, 239–251 | 모듈마다 `dotnet … ids verify` 프로세스 하나, 그다음 `ids verify-set` 하나 |
| 게시 | `MetadataRunner.cs:254`, `MetadataOutputTransaction.cs:163` | 바뀐 파일만 쓴다(같은 byte면 쓰지 않는다 — 게시는 이미 증분이다) |
| 셰이더 파라미터 | `Program.cs:356`, `ShaderParameterRunner.cs:28–72` | 셰이더 파라미터 모듈마다 `dotnet … shader-parameters`(65행). MetaCompiler 안에서 clang AST(`ShaderParameterGenerator.cs:267`)와 탐침(662행)을 돈다 |
| MetaCompiler 한 번 | `Rocks.MetaCompiler/CompilerDriver.cs` | 환경 탐색(35행 — 넘겨받은 환경에 `VCToolsInstallDir`가 있어 VsDevCmd는 건너뛰지만 `clang -print-resource-dir` 탐침은 돈다), 모듈의 리플렉션 헤더 전체를 한 TU로 `-ast-dump=json`(100행), 게임 조합으로 다시(139행), 정규 형식·closure·Binds·Within 탐침(157–179행), native 멤버 탐침(240행), `clang-format --version`(266행)과 생성 파일마다 `clang-format` 프로세스(274–285행 — 차례로), 생성 헤더마다 검증 컴파일(348–362행, `--jobs` 만큼 병렬) |

- 정리: 입력(리플렉션 헤더와 그 include, 정의 헤더, 인자, 도구 판, 안정 ID 명세)이 그대로여도 25개 모듈 × (clang 수 회 + clang-format 수십 회 + 검증 컴파일 수십 회)와 ID 검사 26회가 매 빌드 돈다. 출력은 byte가 같으므로 게시는 아무것도 바꾸지 않는다 — 결과적으로 버리는 일이다.
- [리플렉션 기반](../ReflectionFoundations.ko.md) 118행이 "탐색은 빌드마다 한다 … 필요해지면 캐시"라고 미뤄 둔 것이 지금 비용의 대부분이 되었다.
- UE는 UHT 입력 헤더가 timestamp 파일보다 새롭지 않으면 UHT를 건너뛴다(`UBT/System/UHTExecution.cs:896` `AreGeneratedCodeFilesOutOfDate`, 1394행, 1518행 "Generated code is up to date.").

### 1.5 원인 2: 헤더 이름 목록이 native key를 과하게 바꾼다

- native 선언 입력 key는 include·소스 디렉터리의 **헤더 이름 목록 전체**를 담는다(`ActionGraph.cs` `NativeAction.Key`의 `header-directories`). 목적은 새 shadow header·`__has_include` 변화·삭제를 잡는 것이다.
- 그 디렉터리 아래 어디에든 헤더가 하나 생기거나 없어지면 그 디렉터리를 보는 모든 TU가 miss다. `Core/Source/Runtime/Core/Public`은 거의 모든 모듈의 include 디렉터리라서, Core에 헤더를 하나 더하는 commit을 병합하면 내용이 같은 TU까지 모두 다시 컴파일된다.
- 실측: 새 작업 사본 Commandlet(26f5f5b24)과 runtime-s7 Commandlet(941a459df, 117 commit 차)의 대리 key 일치는 compile 시간의 66.6%인데, 이름 목록 조건을 걸면 34.3%다. 줄어든 1,435개 action이 모두 `Core/Public` 목록 변화 때문이다.

### 1.6 원인 3: 병합 중 작업 파일 분리가 묶음을 두 번 바꾼다

- 작업 파일 집합은 `git status`가 빈 칸이 아닌 상태로 보고한 모든 파일이다(`Rocks.Build/WorkingSet.cs`). origin 병합 중에는 들어온 파일이 모두 staged `M`이라 작업 파일이 된다.
- material 작업 사본(병합 중 dirty 1,341개)의 빌드는 `AdaptiveSeparated=283`이었다. 그 `native-cache.json`에는 지금 계획에 없는 기록이 900개 있고, 그중 780개가 "전에는 단독 TU였고 지금은 묶음 구성원"인 소스의 object다. 병합 한 번에 해당 묶음을 두 번 컴파일한다.
- UE도 git working set을 그대로 쓴다(`UBT/System/SourceFileWorkingSet.cs:92`). 권장: 병합 진행 중(`MERGE_HEAD` 있음)이면 index에만 바뀐 항목(X 열만 채운 `M`)은 작업 파일로 보지 않는다(UnityBuildPch 7.1절 개정 — 이 문서 범위 밖, 8장).
- IncrementalStore는 계획에 없는 기록을 지우지 않는다(material `native-cache.json` 4.9 MB, 기록 5,335개 중 900개). 출력이 없는 기록을 Save 때 뺀다(SCS1에 넣는다).

### 1.7 원인 4: compile 결과를 작업 사본·타깃 사이에 다시 쓰지 않는다

| 비교 | 대리 key 일치(action 수) | compile 시간 기준 | 이름 목록 조건까지 |
|---|---|---|---|
| 같은 작업 사본, UIDemo ← Sandbox(둘 다 Game 종류, 같은 commit) | 65.3% | 45.9% | 45.9% |
| 같은 작업 사본, UIDemo·Sandbox ← Commandlet | 0% | 0% | 0% |
| 새 작업 사본 Commandlet ← runtime-s7 Commandlet(117 commit 차) | 75.2% | 66.6% | 34.3% |
| 새 작업 사본 Commandlet ← audio-wt Commandlet(f851e045e, 119 commit 차) | 72.7% | 64.6% | 34.3% |
| 새 작업 사본 UIDemo ← 다른 두 작업 사본의 Sandbox | 43.4% | 24.5% | — |

- **타깃 종류가 다르면 0%다.** Commandlet은 `/DROCKS_WITH_EDITOR_DATA=1`, Game 종류는 `=0`이다(타깃 매크로). 이 매크로를 읽지 않는 서드파티 모듈(Sodium·Zstd·Jolt·Opus 등, compile 시간의 34–37%)도 인자가 달라 갈린다. 서드파티에 타깃 매크로를 넘기지 않으면 모든 타깃 종류에서 같아진다(BuildTargets 3.2절 개정 제안, 8장).
- 대리 key는 PCH 바이너리 대신 PCH의 입력을 보므로 위 수치는 실제 저장소의 상한에 가깝다.

### 1.8 출력의 경로 의존(지금)

| 항목 | 지금 상태 | 결과 |
|---|---|---|
| COFF 머리 timestamp | 실제 시각(`/Brepro` 없음 — clang-cl 기본은 `-mincremental-linker-compatible`) | 같은 입력도 실행마다 byte가 다르다 |
| `S_OBJNAME` | `<out>\.native-tmp\<guid>\<이름>.obj`(실행마다 다른 stage 경로) | 실행마다 다르다 |
| `LF_BUILDINFO` | 컴파일러 절대 경로, 작업 디렉터리, `-cc1` 명령줄 전체(`/I`·`/FI`·`/Fp`·`-pch-through-header` 절대 경로) | 작업 사본·타깃마다 다르다 |
| 줄 표·타입의 소스 경로 | 절대 경로 | 작업 사본마다 다르다 |
| `__FILE__` | `-fmacro-prefix-map=<루트>\=`로 루트 기준 상대 경로(이미 있음) | 소스는 같다. 출력 폴더 아래 생성 파일은 `out\<프리셋>\…`이 남는다 |
| ISPC object | timestamp 0, 절대 경로 없음 | 그대로 공유할 수 있다 |
| PCH(`.pch`) | 헤더 절대 경로·강제 include 경로를 담고 소비 TU가 그 경로로 입력을 검증한다 | 경로에 묶인다 |
| 메타 생성물 | 루트 기준 include 철자, 절대 경로 없음(리플렉션 생성 소스 실측) | 그대로 공유할 수 있다 |

### 1.9 확인한 정상 사항

- 무변경 빌드의 native 판정은 hit다: 로그 200회가 `executed≤5`이고, material의 현재 compile 기록 4,189개는 의존 기록이 지금 파일과 모두 맞는다(출력이 있는 기록 전부). native key가 빌드마다 바뀌는 결함, 의존 기록이 늘 실패하는 결함, 파일 ID·mtime 결함은 없었다.
- material의 954초 빌드는 origin 병합(헤더 추가·삭제 361개, 수정 109개 — `Base/Compiler.h` 포함)이 원인이라 native 재컴파일은 맞다. 그 빌드도 954초 중 713초가 메타 단계다.
- 입력이 같은데 PCH가 다시 실행될 때(1.5절의 이름 목록 변화) `.pch` byte가 같은지는 확인하지 못했다. SCS1 시험으로 확인한다(5.1절 T1-6).
