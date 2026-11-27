## 2. 메타 단계 증분(1순위)

### 2.1 목표

- 리플렉션 메타 생성, 셰이더 파라미터 생성, 안정 ID 검사가 **입력이 같으면 프로세스를 띄우지 않고** 이전 생성물을 그대로 쓴다.
- 생성물이 지워졌거나 바뀌었으면 다시 생성한다. 입력이 하나라도 바뀌었으면 그 모듈과, 그 결과를 입력으로 받는 모듈만 다시 생성한다.
- 판정 규칙은 native compile과 같다: 선언 입력 key + 의존 기록 검증 + 출력 검증(UnityBuildPch 4.1–4.3절). 판정 코드를 두 벌 두지 않는다(BR3).
- 출력 의미는 지금과 같다: 다시 생성한 모듈만 한 transaction으로 게시하고, 실패하면 아무것도 게시하지 않으며 기록도 바꾸지 않는다.

### 2.2 메타 action

메타 단계의 일을 action 넷으로 나눈다. 각 action의 기록은 `<out>/Metadata/meta-cache.json`이다(IncrementalStore 형식, 파일 이름만 다르다).

| action Id | 하는 일 | 출력 |
|---|---|---|
| `meta:<모듈>` | MetaCompiler 리플렉션 생성(지금 `MetadataRunner.cs:122–224`의 한 회차) | 생성 Public·Private·Source 디렉터리의 파일 전부, descriptor, binding schema, identity inventory |
| `meta-ids:<모듈>` | `ids verify --manifest … --inventory …`(227–236행) | 없음(성공 기록만) |
| `meta-ids-set` | `ids verify-set`(239–251행) | 없음 |
| `shader-parameters:<모듈>` | MetaCompiler `shader-parameters`(`ShaderParameterRunner.cs:28–72`) | 그 모듈 `Generated/<모듈>/ShaderParameters` 아래 파일 전부 |

- identity inventory는 지금 stage에만 있다(`<stage>/<모듈>.inventory.json`). `meta-ids` 판정의 입력이므로 `<out>/Metadata/<모듈>.inventory.json`으로 게시한다(BR4).

### 2.3 선언 입력 key

`meta:<모듈>`:
- 형식 판(`rocks-meta-v1`)과 **MetaCompiler 도구 ID**: `HostTool(root, "Rocks.MetaCompiler")`가 가리키는 도구 디렉터리의 파일(어셈블리·`*.deps.json`·`*.runtimeconfig.json`·`.clang-format` — `CompilerDriver.cs:280`이 `AppContext.BaseDirectory`에서 읽는다) 경로와 내용 해시. descriptor·binding schema·생성 형식 판은 이 도구 안의 상수라 도구 ID에 든다.
- **Clang 툴체인 key**(`Fingerprints.Toolchain` — 컴파일러 내용 digest·keyed 환경)와 **clang-format 판**: `ROCKS_CLANG_FORMAT`(없으면 PATH의 `clang-format`)를 푼 실행 파일 경로와 내용 해시.
- **응답 파일 인자 전체**(`MetadataRunner.cs:129–193`이 만드는 목록). stage 경로는 자리 표시로 바꾼다: `--output-root`·`--descriptor`·`--binding-schema`·`--identity-inventory`의 stage 경로 → `<STAGE>/…`, `--include-dir`가 가리키는 다른 모듈의 stage 생성 루트 → 그 모듈의 게시 경로(내용은 아래 import로 든다). `--clang-arg`(FP 인자·정의·언어 인자), `--require-generated-body`, `--verify-sources`는 그대로 든다. `--jobs`는 출력과 무관하므로 뺀다.
- 입력 헤더(`--input`) 내용과 그 include 철자·private 표지, 정의 헤더(`--definitions-header`) 내용, identity 명세(`--identity-manifest`) 내용.
- import한 descriptor(`--import`) 내용. 의존 모듈이 이번에 다시 생성되면 새 descriptor 내용이, 건너뛰었으면 게시된 descriptor 내용이 든다. 그래서 key는 의존 순서로 계산한다(2.5절).
- include·소스 디렉터리의 헤더 이름 목록 — SCS1 전까지는 native와 같은 규칙, SCS1 뒤에는 정밀 include 검증(3.3.3절)으로 바꾼다.

`shader-parameters:<모듈>`: 위와 같은 구성(도구 ID, 툴체인 key, 인자 전체, `--virtual-root`, 입력 헤더·정의 헤더 내용). 리플렉션 생성 헤더를 include할 수 있으므로 그 내용은 의존 기록으로 든다.

`meta-ids:<모듈>`: 도구 ID, 명세 내용, 게시된 inventory 내용. `meta-ids-set`: 도구 ID, 모든 명세(이번 실행 모듈·예약 명세·import 모듈의 명세 — 239–244행의 목록 그대로) 경로 순 내용.

### 2.4 의존 기록과 출력 검증

- **의존 기록**: MetaCompiler가 띄우는 모든 clang 실행(본 AST·게임 조합·탐침·생성 헤더 검증 컴파일, `shader-parameters`의 AST·탐침)에 depfile 인자(clang-cl `/clang:-MD /clang:-MF<파일>`, clang++ `-MD -MF <파일>`)를 붙이고, MetaCompiler가 그 합집합을 의존 기록(UnityBuildPch 4.1절 형식, `<STAGE>` 아래 경로는 게시 경로로 되돌림)으로 낸다. 툴체인 소유 헤더는 뺀다(native와 같다). `--input` 헤더가 include한 Core 헤더가 바뀌면 이 기록이 잡는다.
- 입력이 표준 입력(`-`)인 clang 실행도 depfile은 연 파일 전부를 적는다(입력 자체는 `<stdin>`이고 key에 이미 있다).
- **출력 검증**: 기록한 출력 파일마다 (크기, 수정 시각, 파일 ID)가 같으면 기록 해시를 믿고, 다르면 내용을 다시 해시한다(UnityBuildPch 4.3절). 게시 디렉터리에 기록에 없는 파일이 있어도 miss다(지금 게시가 잉여 파일을 지우는 규칙 — `MetadataRunner.cs:288–330`과 같은 결과가 나오게).
- 판정: 선언 입력 key가 같고, 의존 기록이 모두 맞고, 출력이 모두 기록대로면 hit다. hit면 출력 줄을 `Rocks metadata: module X: up to date`로 낸다.

### 2.5 실행 흐름

1. 도달하는 메타 모듈을 의존 순서로 둔다(지금과 같다).
2. 모듈마다(의존이 끝난 것부터) key를 계산하고 판정한다. hit면 그 모듈의 "stage"는 게시본이다: 의존 모듈의 `--include-dir`·`--import`가 게시 경로를 가리킨다(내용이 기록과 같다는 것을 방금 확인했다).
3. miss면 지금처럼 stage에 생성한다.
4. `meta-ids:<모듈>`은 inventory(이번 생성본 또는 게시본)와 명세로 판정하고, miss면 `ids verify`를 돈다. 모든 모듈 뒤 `meta-ids-set`도 같다.
5. 다시 생성한 모듈이 있으면 그것만 `MetadataOutputTransaction.Publish`로 한꺼번에 게시한다. 게시가 성공한 뒤에만 그 모듈들의 기록(key·의존 기록·출력 해시)을 저장한다. 하나라도 실패하면 아무것도 게시하지 않고 기록도 바꾸지 않는다(다음 빌드가 다시 시도한다).
6. 다시 생성한 모듈이 없으면 stage 디렉터리도 만들지 않는다.
7. `shader-parameters:<모듈>`도 같은 판정이다. MetaCompiler가 출력 루트에 직접 쓰므로(`ShaderParameterGenerator.cs:1298–1305` `WriteIfChanged`) 실행 뒤 출력 목록과 해시를 기록한다.

- 셰이더 파라미터 생성 시간을 evidence에 따로 적는다(`shader-parameters` 단계). 메타 모듈마다 시간과 hit 여부를 action 보고처럼 남긴다(`<out>/Metadata/meta-report.json` — 모듈, hit, 소요 시간, miss 원인).

### 2.6 병렬(MI3)

- 의존이 끝난 모듈끼리는 동시에 돈다. 동시 모듈 수 × 모듈 안 검증 병렬(`--jobs`, `CompilerDriver.cs:348`)이 빌드의 `--jobs`를 넘지 않게 나눈다(동시 모듈이 k개면 모듈마다 `max(1, jobs / k)`).
- 출력 순서는 결정적으로 유지한다: 모듈 결과 줄은 의존 순서대로 모아 낸다(완료 순서가 아니라).
- 효과는 모듈별 시간(MI0)을 잰 뒤 임계 경로로 계산한다. MaterialSchema 하나가 수 분이면 그것이 하한이다.

### 2.7 MetaCompiler 안의 고정 비용(MI4)

MI0 측정에서 비중이 크면 고친다(측정 전에는 정하지 않는다).
- 생성 파일마다 `clang-format` 프로세스를 하나씩 띄운다(`CompilerDriver.cs:274–285`). 모듈의 생성 파일을 임시 파일로 써서 한 번에 넘기면 프로세스가 하나가 된다.
- `clang-format --version`(266행)과 `clang -print-resource-dir` 탐침(`NativeEnvironment.cs:222`)은 MetaCompiler 실행마다 돈다. 빌드가 한 번 확인한 값을 인자로 넘길 수 있다.
- 본 AST(100행)와 게임 조합(139행)이 같은 헤더를 두 번 파싱한다. 조건 블록 검사 목적이라 줄일 수 있는지는 리플렉션 문서에서 따로 정한다.

### 2.8 버린 대안

- UE처럼 타깃 전체를 timestamp 하나로 판정: 모듈 하나만 바뀌어도 25개가 모두 돈다. 시각만으로 판정하지 않는다는 Rocks 규칙(UnityBuildPch 4.1절)과도 맞지 않는다.
- 입력 헤더 내용만 key로(의존 기록 없이): 리플렉션 헤더가 include한 Core 헤더의 타입 변화가 생성물에 들어가는데 그것을 놓친다.
- 메타 생성을 native action 그래프에 합쳐 한 번에 계획: 생성물이 native 계획의 입력(생성 소스 목록·include 디렉터리)이라 계획 전에 끝나야 한다. 지금 단계 구조를 유지한다.
