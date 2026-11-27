## 5. 시험·단계·측정

### 5.1 시험

모든 시험은 결정적이다. 경합 순서는 시간이 아니라 명시 gate(시험 hook·pipe)로 고정한다. 시험 hook(`CompileStoreHooks` — 이동 직전, 이동 결과 판정 직전, 정리의 삭제 직전, `MetadataStepHooks` — 모듈 실행 직전·게시 직전)은 제품 코드가 걸지 않는다([에셋 너겟](../AssetSystem/09-AssetNuggets.ko.md) A3b와 같은 방식).

| 묶음 | 내용 |
|---|---|
| T5 메타 증분 | 실제 MetaCompiler로 작은 고정 모듈 묶음(의존 사슬 A→B→C와 무관한 D, 셰이더 파라미터 모듈 하나)을 쓴다. T5-1 두 번째 실행은 MetaCompiler·`ids` 프로세스를 하나도 띄우지 않는다(실행 수를 hook으로 센다) — 모든 모듈 `up to date`. T5-2 A의 리플렉션 헤더 수정 → A와, descriptor가 바뀌었을 때만 B·C가 다시 돈다. D는 돌지 않는다. T5-3 A의 리플렉션 헤더가 include한 일반 헤더 수정 → 의존 기록이 그 헤더를 가진 모듈만 다시 돈다. T5-4 게시된 생성 파일 삭제·byte 변경·잉여 파일 추가 → 그 모듈 재생성과 복원. T5-5 정의(모듈 define)·`--clang-arg`(FP 인자) 하나 변경 → miss. T5-6 MetaCompiler 도구 디렉터리 파일 하나 변경 → 모두 다시. T5-7 clang-format 실행 파일 변경(`ROCKS_CLANG_FORMAT`을 다른 사본으로) → 모두 다시. T5-8 identity 명세 변경 → 그 모듈 재생성과 `ids verify`·`verify-set`. T5-9 두 루트에서 생성물 byte가 같다(저장소 공유 전제). T5-10 기록 손상·형식 판 다름 → 다시 생성. T5-11 `--verify-metadata-sources` 켜고 끔 → miss. T5-12 한 모듈 실패 → 아무것도 게시하지 않고 기록도 그대로, 다음 실행이 다시 시도. T5-13 병렬: 의존 모듈이 끝나기 전에 소비 모듈이 시작하지 않는다(gate), 출력 줄 순서가 의존 순서다. T5-14 셰이더 파라미터: 두 번째 실행 건너뜀, 리플렉션 생성 헤더 변경 → 다시, 출력 삭제 → 다시 |
| T1 key·검증 | T1-1 정규화: 루트·출력 폴더 접두만 바뀌고 `C:\work\rocks2` 같은 접두 겹침, 대소문자, `/I`·`/FI`·`-I` 다음 인자·`-fmacro-prefix-map` 안 경로가 맞게 바뀐다. 루트만 다른 두 계획의 1차 key가 같다. ISPC stage 상대 실행과 무관하다. T1-2 인자 하나만 달라도 miss: `-ffp-contract=off` 제거, `/O2`↔`/Od`, `/MD`↔`/MDd`, `ROCKS_WITH_EDITOR_DATA` 값, 정의 헤더의 `JPH_ENABLE_ASSERTS` 한 줄, 서드파티 `platformDefines`(`ASTCENC_SSE=20`↔`41`), ISPC 판·digest·고정 인자. T1-3 리플렉션 생성 소스·생성 헤더 byte 하나 변경 → miss(입력·의존 기록 두 경로 각각). T1-4 정밀 include 검증: 앞선 탐색 디렉터리에 같은 이름 생성 → miss, include 스택 디렉터리에 같은 이름 생성 → miss, 연 적 없는 헤더 추가 → hit, 연 헤더 삭제 → miss, `__has_include` 대상 생성·삭제 → miss, 매크로 `__has_include`·`#include_next` → 이름 목록 방식. T1-5 지역 증분이 같은 규칙: Core/Public에 무관한 헤더를 더해도 무변경 TU가 다시 컴파일되지 않는다. T1-6 같은 입력으로 PCH를 다시 실행하면 `.pch` byte가 같다(다르면 원인을 기록하고, 소비 key를 PCH 입력 key로 둔 이 설계가 그 영향을 막는지 확인) |
| T2 결정성 | 실제 툴체인으로 두 임시 루트(긴 이름·비 ASCII 포함)에서 같은 소스 묶음을 컴파일해 object byte가 같다(Debug·Development·Release·Asan·Coverage, Windows. macOS·Android는 그 기계에서). 통과한 구성만 경로 독립이다. T2-2 `__FILE__`이 지금과 같다(소스), 생성 헤더는 `out\Generated\…`. T2-3 매크로 매핑 우선순위. T2-4 다른 루트의 object를 링크한 PDB에 링크한 쪽 절대 경로가 있다 |
| T3 저장소 | 2단 조회(후보 여럿 중 맞는 것, 모두 틀림), 게시·hit 왕복, 같은 후보 동시 게시(두 자식 프로세스 gate — 하나만 남고 둘 다 성공), blob만 있고 항목 없음, 항목만 있고 blob 없음(항목 삭제·miss), 항목 손상(본문 해시·JSON·echo·크기 각각), blob 손상(byte 하나·잘림) → 삭제·miss, 읽는 중 정리 삭제(gate), 정리 대 게시(gate), 후보 상한, 크기 상한·사용 시각 순·같은 시각이면 이름 순, 잠금을 다른 프로세스가 쥠 → 기다리지 않고 건너뜀, 읽기 전용 루트(조회만), 디스크 가득·권한 오류 주입 → miss·빌드 성공, 연속 IO 오류 → 저장소 끔, 꺼짐·읽기 전용 설정 |
| T4 통합 | 가짜 컴파일러(입력을 출력으로 베끼는 시험 도구)로: 작업 사본 A 빌드 → B 빌드가 모두 restored, B의 지역 기록으로 다음 빌드 hit, B에서 헤더 수정 → 그 TU만 실행, 저장소 off면 지금과 같은 결과, 빌드 증거의 원본 안정 검사 유지. 메타 결과 공유(SCS5): A의 메타 단계 뒤 B의 메타 단계가 MetaCompiler를 띄우지 않는다 |

- 시험 위치: 단위·저장소 시험은 `Programs/Rocks.Build.Tests`, 메타 증분과 실제 툴체인 결정성 시험은 `Programs/Rocks.Build.IntegrationTests`.

### 5.2 단계

메타 단계(1순위)를 먼저 한다. SCS1은 메타 증분의 include 규칙에도 쓰이므로 MI 단계와 이어서 한다.

| 단계 | 내용 | 파일 |
|---|---|---|
| MI0 | 메타 단계 계측: 모듈별 MetaCompiler 시간·`ids` 시간·셰이더 파라미터 시간을 `<out>/Metadata/meta-report.json`과 evidence 단계(`shader-parameters`)로 남긴다. 기존 빌드 한 번으로 모듈별 시간표를 얻는다 | `MetadataRunner.cs`, `ShaderParameterRunner.cs`, `Program.cs`(evidence) |
| MI1 | 메타 action 판정: `meta:<모듈>`·`meta-ids:<모듈>`·`meta-ids-set`의 key·의존 기록·출력 검증·기록, 건너뛴 모듈의 게시본 참조, 바뀐 모듈만 게시, inventory 게시. MetaCompiler가 clang 실행마다 depfile을 받아 합친 의존 기록을 낸다. 판정 코드는 Rocks.Build의 공개 API로 낸다(BR3) | `MetadataRunner.cs`, `MetadataOutputTransaction.cs`, `Rocks.MetaCompiler/CompilerDriver.cs`(depfile), `Rocks.Build`(`IncrementalStore`·`DependencyRecord`·`Fingerprints`를 감싼 공개 판정 API) |
| MI2 | `shader-parameters:<모듈>` 판정 | `ShaderParameterRunner.cs`, `Rocks.MetaCompiler/ShaderParameterGenerator.cs`(depfile) |
| MI3 | 의존 DAG 병렬 실행, 검증 병렬 나눔, 결정적 출력 순서 | `MetadataRunner.cs` |
| MI4 | MI0 결과에 따라 MetaCompiler 고정 비용(모듈당 clang-format 한 번, 판 확인 인자 전달) | `Rocks.MetaCompiler/CompilerDriver.cs` |
| SCS1 | 정밀 include 검증·의존 기록 형식 2·`__has_include` 기록, 지역 key에서 헤더 이름 목록 제거(native·메타 공통), IncrementalStore의 출력 없는 기록 정리 | `ActionGraph.cs`(Key), `BuildInputs.cs`(디렉터리 이름 집합), `Dependencies.cs`, `Executor.cs`(기록), `IncrementalStore.cs`, UnityBuildPch 4.1절 개정 |
| SCS2 | 경로 독립 object: 3.4.1절 인자, `ObjectOf` 루트 기준 해시, link `/pdbsourcepath`, 도구 빌드 ID·compile DB·IntelliSense 정규화 | `ActionGraph.cs`(`TargetCompileArguments`·`ObjectOf`·link), `Pch.cs`, `BuildInfo.cs`, `ProjectFiles/IntelliSenseArguments.cs`, `CompilationDatabase.cs` |
| SCS3 | 저장소 본체: 배치·1차 key 정규화·항목 형식·blob·게시·조회·정리·hook | `Programs/Rocks.Build/CompileStore/`(`StoreLayout`·`StoreKey`·`StoreEntry`·`StoreBlobs`·`StoreCollect` — 클래스당 파일) |
| SCS4 | native 연결: 판정 순서, 복원, 게시, 보고·요약 줄, 설정(`ROCKS_COMPILE_STORE`·`--compile-store`·`ROCKS_COMPILE_STORE_DIR`·`ROCKS_COMPILE_STORE_MAX_GB`) | `NativeBuildEngine.cs`, `Executor.cs`, `NativeBuildModels.cs`(보고), `Rocks.Build.Cli/Program.cs` |
| SCS5 | 메타 결과 공유(3.7절) | `MetadataRunner.cs`, `ShaderParameterRunner.cs` |
| SCS6 | 구성·플랫폼별 결정성 확인(T2)과 경로 독립 목록 확장, 끝 측정(5.3절)과 문서 개정 | `NativeCompileProfile.cs`·시험·문서 |

### 5.3 측정

빌드 기계가 다른 빌드를 돌리지 않을 때(한 번에 한 builder) 같은 순서로 두 번 잰다. 값은 `build-evidence.json`의 단계 시간(prerequisite·metadata·shader-parameters·native), `meta-report.json`의 모듈별 시간·hit, action 보고의 `restored`·miss 원인이다.

1. S0(MI1 전후): wt-ut5 측정과 같은 조작 — 작업 사본에서 `.cpp` 하나(리플렉션과 무관)만 고친 뒤 Sandbox Debug 증분 빌드. 지금 523초. MI1 뒤 메타 단계가 수 초, 전체가 약 1분이어야 한다. 무변경 EngineTests Debug(지금 572–720초)도 같이 잰다.
2. S0b(MI3 전후): 리플렉션 헤더를 많이 바꾼 상태(예: origin 병합 직후)의 메타 단계 시간. MI0의 모듈별 시간으로 계산한 임계 경로와 비교한다.
3. S1: 빈 저장소, 새 작업 사본(commit C)에서 Commandlet Debug. 기준값.
4. S2: 두 번째 새 작업 사본(같은 C)에서 Commandlet Debug. 기대: compile action 95% 이상 restored, 메타 모듈 전부 저장소 hit(SCS5 뒤), S1과 object byte가 같다(T2의 실제 규모 확인).
5. S3: 새 작업 사본(C 뒤 하루치 origin)에서 Commandlet Debug. 1.7절의 34%(지금 include 규칙)·67%(상한)와 비교한다.
6. S4: S3 작업 사본에서 UIDemo 뒤 Sandbox. 1.7절의 46%와 비교한다.
7. S5: S3 작업 사본에 Core/Public 헤더 하나를 더하는 commit을 만들고 다시 빌드. SCS1 전에는 거의 전부, 뒤에는 0에 가깝게 실행되어야 한다.
