## 2. 메타 단계 증분(1순위)

### 2.1 목표

- 리플렉션 메타 생성, 셰이더 파라미터 생성, 안정 ID 검사가 **입력이 같으면 프로세스를 띄우지 않고** 이전 생성물을 그대로 쓴다.
- 생성물이 지워졌거나 바뀌었으면 다시 생성한다. 입력이 하나라도 바뀌었으면 그 모듈과, 그 모듈의 생성물을 실제로 읽은 모듈만 다시 생성한다.
- 판정 규칙은 native compile과 같다: 선언 입력 key + 의존 기록 검증 + 출력 검증(UnityBuildPch 4.1–4.3절). 판정 코드를 두 벌 두지 않는다(BR3).
- 출력 의미는 지금과 같다: 다시 생성한 모듈만 한 transaction으로 게시하고, 실패하면 아무것도 게시하지 않으며 기록도 바꾸지 않는다.
- 빌드 중 입력이 바뀌었으면 그 실행의 기록을 저장하지 않는다(2.6절).
- 건너뛰기는 **shadow mode로 먼저 켠다**(2.7절): 판정만 하고 늘 생성해, "건너뛸 수 있다고 판정했는데 생성물 byte가 다르다"가 실제 빌드에서 0건인지 확인한 뒤에 건너뛰기를 켠다.

### 2.2 메타 action

| action Id | 하는 일 | 출력 |
|---|---|---|
| `meta:<모듈>` | MetaCompiler 리플렉션 생성(`MetadataRunner.cs:122–224`의 한 회차)과 그 모듈의 `ids verify`(227–236행) | 생성 Public·Private·Source 디렉터리의 파일 전부, descriptor, binding schema, identity inventory |
| `meta-ids-set` | `ids verify-set`(239–251행) | 없음(성공 기록만) |
| `shader-parameters:<모듈>` | MetaCompiler `shader-parameters`(`ShaderParameterRunner.cs:28–72`) | 그 모듈 `Generated/<모듈>/ShaderParameters` 아래 파일 전부 |

- 모듈의 `ids verify`는 `meta:<모듈>`에 합친다. 입력(명세)은 key에, inventory는 출력에 있으므로 `meta:<모듈>`이 hit이면 `ids verify`의 입력도 지난번과 같다.
- identity inventory는 지금 stage에만 있다(`<stage>/<모듈>.inventory.json`). 출력 검증 대상이므로 `<out>/Metadata/<모듈>.inventory.json`으로 게시한다(BR4).
- 기록 위치: key·출력 해시는 `<out>/Metadata/meta-cache.json`(IncrementalStore와 같은 형식, 파일 이름만 다르다), 의존 기록은 모듈마다 `<out>/Metadata/<모듈>.meta.deps`, 셰이더 파라미터는 `<out>/Metadata/<모듈>.shader-parameters.deps`다.
- 전제: 같은 출력 폴더의 빌드 호출은 `OutputInvocationLock`으로 하나뿐이다. 그래서 기록 파일의 쓰는 쪽은 하나다.
- MetaCompiler `--check`(게시본이 최신인지 보기만 하는 모드)와 `ids` 명령의 쓰기 모드는 기록을 읽지도 쓰지도 않는다.

### 2.3 선언 입력 key

`meta:<모듈>`:
- 형식 판(`rocks-meta-v1`)과 **MetaCompiler 도구 ID**: 넘겨받은 `compilerTool`(엔진 빌드 `HostTool(root, "Rocks.MetaCompiler")`, 설치 SDK 게임 빌드 `<sdk>/Tools/Rocks.MetaCompiler/Rocks.MetaCompiler.dll` — `InstalledSdk.cs:455`)이 있는 **디렉터리 전체**(하위 `runtimes/`·위성 어셈블리 디렉터리 포함, 상대 경로와 내용 해시)와 **dotnet 공유 런타임 판**(`dotnet --list-runtimes` 출력 — 도구의 `*.runtimeconfig.json`과 함께 실제로 묶일 런타임을 정한다, 호출마다 한 번 실행). descriptor·binding schema·생성 형식 판은 이 도구 안의 상수라 도구 ID에 든다.
- **Clang 툴체인 key**(`Fingerprints.Toolchain`)와 **clang-format**: Cli가 호출마다 한 번 `ROCKS_CLANG_FORMAT`(없으면 툴체인 환경의 PATH)을 실행 파일 경로로 풀어 MetaCompiler에 `--clang-format <경로>`로 넘기고, 그 경로와 내용 해시가 key에 든다. MetaCompiler는 받은 경로만 쓴다(스스로 다시 찾지 않는다).
- **응답 파일 인자 전체**(`MetadataRunner.cs:129–193`이 만드는 목록). stage 경로는 자리 표시로 바꾼다: `--output-root`·`--descriptor`·`--binding-schema`·`--identity-inventory`의 stage 경로 → `<STAGE>/…`, `--include-dir`가 가리키는 다른 모듈의 stage 생성 루트 → 그 모듈의 게시 경로, `--import`의 stage descriptor → 그 모듈의 게시 descriptor 경로(내용은 아래 import로 든다). `--clang-arg`(FP 인자·정의·언어 인자), `--require-generated-body`, `--verify-sources`는 그대로 든다. `--jobs`와 계측 인자(`--phase-report`·`--dependency-record`)는 출력과 무관하므로 뺀다.
- **게시 위치 목록**(`MetadataLayout`이 정한 이 모듈의 생성 Public·Private·Source 디렉터리, descriptor, binding schema 경로).
- 입력 헤더(`--input`) 내용과 그 include 철자·private 표지, 정의 헤더(`--definitions-header`) 내용, identity 명세(`--identity-manifest`) 내용.
- import한 descriptor(`--import`) 내용. 의존 모듈이 이번에 다시 생성되면 새 descriptor 내용이, 건너뛰었으면 게시된 descriptor 내용이 든다. 그래서 key는 의존 순서로 계산한다.

`shader-parameters:<모듈>`: 위와 같은 구성(도구 ID, 런타임 판, 툴체인 key, clang-format, 인자 전체, `--virtual-root`, 출력 루트, 입력 헤더·정의 헤더 내용). 리플렉션 생성 헤더를 include할 수 있으므로 그 내용은 의존 기록으로 든다.

`meta-ids-set`: 도구 ID, 런타임 판, 모든 명세(이번 실행 모듈·예약 명세·import 모듈의 명세 — 239–244행의 목록 그대로) 경로 순 내용.

### 2.4 의존 기록

MetaCompiler가 `--dependency-record <파일>`로 다음 셋을 낸다(경로만 — 내용 해시는 Cli가 입력 메모로 잰다, 2.6절).

1. **clang이 연 파일**: MetaCompiler 안의 clang 실행은 모두 wrapper 하나(`ClangInvocation`)를 거친다. wrapper는 실행마다 depfile 인자(clang-cl `/clang:-MD /clang:-MF<파일>`, clang++ `-MD -MF <파일>`)를 붙이고, 실행 뒤 depfile이 없거나 읽을 수 없으면 그 실행을 실패로 다룬다(RMC 오류 — 의존을 모르는 생성물을 기록하지 않는다). 실행 수와 읽은 depfile 수를 함께 적는다(시험이 같은지 본다). 대상은 본 AST(`CompilerDriver.cs:100`), 게임 조합(139행), 정규 형식·closure·Binds·Within 탐침(157–179행), native 멤버 탐침(240행), 생성 헤더 검증 컴파일(348–362행), `shader-parameters`의 AST(`ShaderParameterGenerator.cs:267`)와 탐침(662행)이다. 실측: clang-cl은 `-fsyntax-only`·`-ast-dump=json`·표준 입력(`-`)에서도 depfile을 쓰고, 표준 입력 자체는 적지 않는다(target `-.o`, 연 헤더만).
2. **열거**: clang 밖에서 디렉터리를 열거한 것 — 열거한 디렉터리, 패턴, 하위 포함 여부, 결과 경로 집합. 대상은 `ModulePublicHeaders`(`CompilerDriver.cs:829–870`), 엔진 모듈 디렉터리 열거(53행).
3. **존재 확인**: clang 밖에서 `File.Exists`로 찾은 경로와 결과(참·거짓). 대상은 `ResolveHeader`(116–124행), `ValidateIncludeBinding`(1214–1217행), `CanonicalPath.Resolve`. UE UHT도 외부 의존 파일을 따로 기록한다(`UBT/System/UHTExecution.cs:1042–1046` `AreExternalDependenciesOutOfDate`).

제외·변환 규칙(MetaCompiler가 낼 때 적용한다):
- 표준 입력(`<stdin>`·`-`)은 적지 않는다(내용은 key의 입력 헤더 목록이다).
- 검증 임시 디렉터리(`.rocks-meta-validation-<guid>` 아래 — `check-*.cpp`, 검증용 생성물 사본)와 이 실행 자신의 출력 루트(`--output-root` 아래)는 적지 않는다. 자기 출력은 입력이 아니다.
- 다른 모듈의 stage 생성 루트 아래 파일은 Cli가 기록할 때 그 모듈의 게시 경로로 바꾼다(MetaCompiler는 받은 경로 그대로 낸다).
- 툴체인 소유 헤더는 Cli가 뺀다(native와 같은 규칙 — `BuildInputs.IsToolchainOwned`).
- 적은 경로가 모두 위 규칙으로 설명되지 않으면(규칙 밖의 임시 경로) 그 모듈은 기록하지 않고 원인을 로그에 남긴다.

include 해석: SCS1 전까지는 include·소스 디렉터리의 헤더 이름 목록 digest를 의존 기록에 함께 적는다(native와 같은 규칙). SCS1 뒤에는 정밀 include 검증(3.3.3절)으로 바꾼다.

### 2.5 판정

선언 입력 key가 같고, 아래가 모두 맞으면 hit다.

- **의존 내용**: 기록한 파일마다 지금 내용 해시가 기록과 같다. 그 파일이 **이번 실행에서 다시 생성한 의존 모듈의 게시 경로** 아래면 게시본이 아니라 그 모듈의 stage 파일과 대조한다(native가 같은 plan의 산출물을 기록 해시로 보는 것과 같은 규칙 — 게시는 판정 뒤에 일어나므로 게시본은 낡았다). 이름 목록·열거·존재 확인도 같은 치환으로 stage에서 다시 본다.
- **열거·존재 확인**: 같은 열거의 결과 집합이 같고, 같은 경로의 존재 여부가 같다.
- **출력**: 기록한 출력 파일마다 (크기, 수정 시각, 파일 ID)가 같으면 기록 해시를 믿고, 다르면 내용을 다시 해시한다(UnityBuildPch 4.3절). 게시 디렉터리에 기록에 없는 파일이 있어도 miss다(지금 게시가 잉여 파일을 지우는 규칙 — `MetadataRunner.cs:288–330`과 같은 결과가 나오게).
- 그래서 의존 모듈 A가 다시 생성되면: A의 descriptor가 같더라도 B가 A의 생성 헤더를 include했고 그 byte가 바뀌었으면 B는 다시 돈다. B가 A의 파일을 하나도 열지 않았고 import한 descriptor도 같을 때만 B가 돌지 않는다.
- hit 줄은 `Rocks metadata: module X: N types; up to date`다(형식 수는 기록에 둔다).
- miss 원인은 하나로 정해 적는다: 기록 없음, 형식 판 다름, key 다름(어느 항목 — 도구·런타임·툴체인·clang-format·인자·게시 위치·입력 헤더·정의 헤더·명세·import), 의존 내용 다름(첫 경로), 열거·존재 다름, 출력 없음·다름·잉여.

### 2.6 입력 메모와 안정 검사

- **입력 메모**: 빌드 호출 시작 때 하나 만들어 메타 단계와 native 엔진이 함께 쓴다(Rocks.Build의 공개 `InputMemo` — native `BuildInputs`가 같은 메모를 씨앗으로 받는다). 항목은 (경로, 내용 해시, 그때의 (크기, 수정 시각, 파일 ID))이고, 다시 쓸 때는 지금 (크기, 시각, 파일 ID)가 같을 때만 메모 해시를 쓴다(다르면 다시 읽는다). 열거·존재 확인 결과도 메모한다.
- **안정 검사**: 메타 단계(셰이더 파라미터까지) 끝에 이번 실행이 판정·기록에 쓴 것을 모두 다시 본다 — 입력 헤더, 이름 목록, 의존 기록의 파일, 열거·존재 확인, import descriptor, 정의 헤더, 명세, **MetaCompiler 도구 디렉터리**(한 작업 사본의 `out/dotnet/bin`은 그 작업 사본의 모든 빌드 호출이 함께 쓰고, 도구를 다시 빌드하는 호출이 동시에 있을 수 있다), clang-format, 툴체인 key. 하나라도 시작 때와 다르면 **이번 실행의 기록을 하나도 저장하지 않고** 원인(첫 경로)을 로그에 남긴다. 게시한 생성물은 그대로 둔다(다음 실행이 miss로 다시 생성한다).
- 빌드 증거(`build-evidence.json`)의 원본 안정 검사(종료 1)는 그대로다. 위 검사는 그것과 별개로 기록의 정합성을 지킨다.

### 2.7 실행 흐름과 shadow mode

모드는 `ROCKS_META_INCREMENTAL`이다: `off`(판정·기록 없음 — 지금과 같다), `shadow`(MI1 기본), `on`(건너뛰기 — shadow 확인 뒤에 켠다). 다른 값은 오류다.

1. 도달하는 메타 모듈을 의존 순서로 둔다(지금과 같다).
2. 모듈마다(의존이 끝난 것부터) key를 계산하고 판정한다.
3. `on`이고 hit면 그 모듈의 "stage"는 게시본이다: 의존 모듈의 `--include-dir`·`--import`가 게시 경로를 가리킨다.
4. miss이거나 `shadow`면 지금처럼 stage에 생성한다. `shadow`에서 hit로 판정한 모듈은 생성 뒤 stage 출력과 기록한 출력 해시를 대조해, 다르면 `Rocks metadata: module X: shadow would-hit but output differs: <첫 파일>`을 내고 수를 센다(`meta-report.json`의 `shadowMismatches`). 이 수가 실제 빌드에서 0이어야 `on`으로 바꾼다.
5. 다시 생성한 모듈의 `ids verify`를 돌고, `meta-ids-set`을 판정한다.
6. 다시 생성한 모듈이 있으면 그것만 `MetadataOutputTransaction.Publish`로 한꺼번에 게시한다.
7. 셰이더 파라미터(`shader-parameters:<모듈>`)를 같은 방식으로 판정·실행한다. MetaCompiler가 출력 루트에 직접 쓰므로(`ShaderParameterGenerator.cs:1298–1305` `WriteIfChanged`) 실행 뒤 출력 목록과 해시를 잰다.
8. 안정 검사(2.6절)를 통과하면 이번에 생성·확인한 모든 action의 기록을 저장한다. 실패·취소·불안정이면 저장하지 않는다.
9. 다시 생성한 모듈이 없으면(`on`) stage 디렉터리도 만들지 않는다.

### 2.8 계측(MI0)

- evidence에 `shader-parameters` 단계를 더한다(지금은 metadata 단계와 native 단계 사이에 기록 없이 돈다).
- `<out>/Metadata/meta-report.json`(형식 판 1): 모듈마다 종류, 벽시계 시간, MetaCompiler 프로세스 CPU 시간·최대 working set(`ProcessSpec.MeasureUsage`), MetaCompiler 안 단계 시간(환경 탐색·본 AST·게임 조합·탐침·AST 해석·clang-format·검증 컴파일·쓰기)과 clang 실행 수·clang 자식의 최대 working set(MetaCompiler `--phase-report <파일>`), 형식 수, 판정(모드·hit·miss 원인), `ids verify` 시간. 단계 전체: `verify-set` 시간, 게시 시간, 안정 검사 결과, shadow 불일치 수.
- 같은 내용을 한 줄 요약으로도 낸다: `Rocks metadata: 25 modules; 0 up to date; 25 generated; 512.3 s`.

### 2.9 병렬(MI3)

- DAG 간선은 `--import`로 받는 모듈과, `--include-dir`가 가리키는 생성 루트의 소유 모듈의 합집합이다(소비 모듈이 그 모듈의 stage 생성물이나 descriptor를 읽는다).
- 동시 모듈 수 = min(`--jobs`, 메모리 상한). 메모리 상한은 MI0이 잰 모듈별 최대 working set(MetaCompiler와 clang 자식의 합)으로 정한다 — 가용 물리 메모리의 절반을 가장 큰 모듈 값으로 나눈 값. 모듈 안 검증 병렬(`--jobs`, `CompilerDriver.cs:348`)은 동시 모듈 수로 나눈다(모듈마다 `max(1, jobs / k)`).
- 결정적 출력과 실패 보고: 모듈 결과 줄은 의존 순서대로 모아 낸다(완료 순서가 아니다). 한 모듈이 실패하면 새 모듈을 시작하지 않고 실행 중인 모듈이 끝나기를 기다린 뒤, 실패한 모듈을 의존 순서로 모두 보고하고 첫 실패의 종료 코드를 쓴다.
- MI3과 MI4의 순서와 효과는 MI0 자료(모듈별 시간·임계 경로·메모리)로 정한다. MaterialSchema 하나가 수 분이면 그것이 하한이다.

### 2.10 MetaCompiler 안의 고정 비용(MI4)

MI0 측정에서 비중이 크면 고친다(측정 전에는 정하지 않는다).
- 생성 파일마다 `clang-format` 프로세스를 하나씩 띄운다(`CompilerDriver.cs:274–285`). 모듈의 생성 파일을 한 번에 넘기면 프로세스가 하나가 된다.
- `clang-format --version`(266행)과 `clang -print-resource-dir` 탐침(`Rocks.Tooling.Common/NativeEnvironment.cs:222`)은 MetaCompiler 실행마다 돈다. Cli가 한 번 확인한 값을 넘길 수 있다.
- 본 AST(100행)와 게임 조합(139행)이 같은 헤더를 두 번 파싱한다. 조건 블록 검사 목적이라 줄일 수 있는지는 리플렉션 문서에서 따로 정한다.

### 2.11 버린 대안

- UE처럼 타깃 전체를 시각으로 판정: UBT는 모듈별 시각 기록(TimestampsFile)을 두고 하나라도 낡았으면 UHT를 돌린다(`UBT/System/UHTExecution.cs:896–990`). 시각만으로 판정하지 않는다는 Rocks 규칙(UnityBuildPch 4.1절)과 맞지 않고, Rocks MetaCompiler는 모듈마다 clang 파싱이라 모듈 단위로 건너뛰어야 이득이 있다.
- 입력 헤더 내용만 key로(의존 기록 없이): 리플렉션 헤더가 include한 Core 헤더의 타입 변화가 생성물에 들어가는데 그것을 놓친다.
- MetaCompiler가 의존 파일의 해시까지 내기: clang이 읽은 뒤 MetaCompiler가 해시하기 전에 파일이 바뀌면 생성물과 다른 내용을 기록한다. 해시는 시작 때 메모로 재고 끝에 안정 검사로 확인한다(2.6절).
- 메타 생성을 native action 그래프에 합쳐 한 번에 계획: 생성물이 native 계획의 입력(생성 소스 목록·include 디렉터리)이라 계획 전에 끝나야 한다.
- 바로 건너뛰기 켬: 의존 기록의 빈틈이 있으면 낡은 생성물을 쓴다. shadow mode로 실제 빌드에서 먼저 확인한다.
