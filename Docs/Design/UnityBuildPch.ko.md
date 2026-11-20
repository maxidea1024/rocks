# 유니티 빌드·PCH 설계

기준일: 2026-11-18, 개정 1. Rocks.Build의 C++ 컴파일 시간을 줄이는 세 가지를 정한다.
- **유니티 빌드**: 여러 `.cpp`를 한 번역 단위로 묶어 컴파일한다.
- **작업 파일 분리**: 편집 중인 파일만 묶음에서 빼서 따로 컴파일한다.
- **PCH**: 미리 컴파일한 헤더다. 모듈 전용과 공유 두 가지를 둔다.

[엔진 구현 계획](EngineImplementationPlan.ko.md)의 다음 요구를 구현 스펙으로 옮긴다.
- 27.2절: action key, include 탐색 검증
- 27.3절: Shared/Explicit PCH, Adaptive Unity, non-unity CI 표본
- 27.8절: I0–I6 반복 시나리오
- 27.9절: M2 말/M3의 PCH/Unity benchmark

대상 코드는 `Programs/Rocks.Build`(ActionGraph·Executor·IncrementalStore·BuildInfo)와 모듈 규칙 `NativeModuleInput`이다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 UnrealBuildTool이다. 경로는 `Engine/Source/Programs/UnrealBuildTool/` 기준이다. UE 이름은 대응표와 근거에만 적는다. 레거시와 deprecated 경로는 따르지 않는다(`bChainPCHs`, `IncludeOrderVersion` 호환, BuildSettingsVersion별 분기, Perforce read-only 판별). clang 동작은 Rocks 툴체인(clang-cl·clang++ 21.1.6)으로 실측했다.

상태: **설계 확정**(결정 UD1–UD20, 2026-11-18 사용자 승인 — 권장안). 리뷰어 교차 검증을 반영했다. 구현 단계는 UB0–UB6(15절)이다. 2026-11-20 사용자 결정으로 착수했고, 단계별 전체 빌드·기준 측정 없이 이어서 구현한 뒤 끝에 한 번 검증한다(12절의 반복 측정은 하지 않는다). 구현 상태와 구현하며 정한 점은 개정 이력에 단계마다 적는다. **UB0–UB6을 구현했다**(2026-11-20). 남은 일은 `unity-profile`과 기준 시나리오 반복 측정(12절), Engine 공유 PCH와 `Object/Object.h` 계열의 PCH 편입(8.4절)이다.

## 1. 현재 상태와 문제

| 항목 | 현재 Rocks.Build | 이 스펙에 주는 영향 |
|---|---|---|
| 번역 단위 | 모듈 소스 하나 = compile action 하나 | 공통 헤더(Core 공개 헤더 등)를 TU마다 다시 파싱한다 |
| 헤더 의존 | compile key가 그 TU가 볼 수 있는 include 디렉터리와 소스 디렉터리의 **헤더 내용 전체**를 해시한다(`ActionGraph.cs:174`, `NativeAction.Key`). README에도 "depfile 기반 정밀 의존 축소는 후속 작업"이라고 적혀 있다 | 헤더 하나를 고치면 그 디렉터리를 보는 모든 TU가 다시 컴파일된다. 작업 파일 분리와 PCH의 이득이 이 비용에 묻힌다 |
| compile identity | **모듈의 전체 소스 목록**이 들어간다(`ActionGraph.cs:223`) | 소스 하나를 추가하거나 지우면 모듈 전체가 다시 컴파일된다 |
| 정의 | 모듈 정의를 명령줄 `/D`·`-D`로 넘긴다(`ActionGraph.cs:200`). AST 생성도 `--clang-arg=/D`로 넘긴다(`Rocks.Build.Cli/MetadataRunner.cs:131`) | 모듈마다 정의가 달라 PCH를 모듈끼리 공유할 수 없다 |
| 툴체인 | Windows clang-cl+lld-link, macOS clang++, Android NDK clang++. MSVC `cl.exe`는 없다 | clang PCH만 다루면 된다. MSVC의 `/Zm`·PDB 공유 문제가 없다 |
| 링크 단위 | 정적 라이브러리 + 실행 파일. DLL은 없다 | 모듈 API 매크로(`*_API`)가 없어 공유 PCH의 정의 충돌이 UE보다 적다 |
| 생성 TU | BuildInfo TU(`<out>/BuildInfo/`)와 규칙 생성 TU(`<out>/GeneratedSources/`). 리플렉션 생성물은 헤더(`.generated.h`)뿐이다. [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md)가 구현되면 리플렉션 생성 소스(`<out>/Generated/<M>/Source/`)가 생긴다 | 지금은 UE의 `.gen.cpp` 묶기 규칙이 필요 없다. 생성 소스는 충돌 없이 생성하므로(그 문서 GSD9) 자기들끼리 묶을지 UB1에서 정한다 |
| 도구 빌드 ID | 실행 파일 도달 집합의 compile·archive·link action **물리 인자**로 만든다. 호스트 경로는 정규화하고, 남으면 예외다(`BuildInfo.cs:448, 618–650`) | 유니티 여부, 작업 파일 집합, PCH 여부에 따라 같은 소스의 ID가 바뀌고, 새 인자(`/FI`·`/Fp`·`-MF`)에서 예외가 난다(8.7절) |
| 소스 규모(2026-11-18) | Core·Engine C++ 729개(런타임 356, 시험 373), 모듈 45개 | 런타임 `.cpp`는 모두 64 KiB 이하다. 평균은 Core 11.7 KiB, ObjectRuntime 15.8 KiB다 |
| 묶음 충돌 위험 | 586개 파일이 익명 namespace를 쓰고 같은 이름이 많다(Core에서 `struct Case` 15회 등). 시험은 FoundationTests·EngineTests 실행 파일 모듈 하나에 모인다 | 충돌 정리(9절)의 작업량이 크다 |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 유니티 빌드 | unity build | 여러 `.cpp`를 `#include`로 묶은 TU 하나로 컴파일한다(업계 공용어) |
| 묶음 TU `UnityBatch` | unity blob(`Module.N.cpp`) | 묶음 하나와 그 생성 파일 `<Module>.Unity.<id>.cpp` |
| 묶음 구간 | (없음) | anchor 두 개 사이의 파일 열. 묶음은 구간 안에서만 만든다 |
| 작업 파일 분리 | adaptive unity build | 편집 중인 파일을 묶음에서 빼서 따로 컴파일한다 |
| 작업 파일 집합 | working set(`ISourceFileWorkingSet`) | git이 수정·추가로 보고한 파일 |
| 모듈 PCH | private PCH(`PrivatePCHHeaderFile`) | 한 모듈 전용 PCH |
| 공유 PCH | shared PCH(`SharedPCHHeaderFile`) | 제공 모듈의 헤더로 만들어 여러 모듈이 쓰는 PCH |
| PCH 변형 | shared PCH variant(접미사 `.NonOpt.Cpp20…`) | 호환되지 않는 compile 인자마다 따로 만든 PCH |
| 정의 헤더 | `Definitions.<Module>.h`·`SharedDefinitions.<X>.h` | 정의를 담아 강제 include하는 생성 헤더 |
| PCH 앞부분 | (through header 앞 강제 include) | PCH를 만들 때 PCH 헤더 앞에 강제 include한 내용. 소비 TU에서도 바이트까지 같아야 한다 |
| 의존 기록 | dependency list(`.d`·`.dep.json`), `CppDependencyCache` | 컴파일러가 실제로 연 헤더와 그 내용 해시 |
| 묶음 충돌 검사 | (없음 — 코딩 규약) | 같은 모듈에서 파일 지역 이름이 겹치는지 보는 정책 검사(9.2절) |
| 검증 빌드 | `-DisableUnity`·`-NoPCH`(`NU` intermediate) | 유니티·PCH를 끈 빌드. 묶음이 감춘 include 누락을 찾는다 |
| 논리 명령 | (없음) | 원본 소스 하나를 단독으로 컴파일하는 명령. compile DB와 도구 빌드 ID의 기준이다 |

## 3. 목표와 범위

목표:
- clean build와 공개 헤더 변경(I4)의 compile 시간을 줄인다. I0(무변경)과 I3(private `.cpp` 하나)의 시간은 늘리지 않는다.
- 묶음 구성과 PCH 선택은 결정적이다. 같은 입력(소스·규칙·작업 파일 집합)이면 같은 action이다. key는 선언 입력 key와 의존 기록 검증의 2단 구조다(4.1절). 같은 파일 상태에서는 같은 판정이 나온다.
- 유니티와 PCH는 compile 속도를 위한 수단일 뿐 의미를 바꾸지 않는다. 검증 빌드(유니티·PCH 끔)가 같은 소스로 통과해야 하며 CI가 이를 유지한다.
- 유니티·PCH·작업 파일 분리는 도구 빌드 ID와 compile DB를 바꾸지 않는다(8.6·8.7절).
- stale hit 0건(계획 27.2)을 유지한다.

범위 밖(별도 결정):
- 분산 실행과 원격 캐시(계획 27.4). 다만 action key는 경로를 정규화한 형태로 유지해 나중에 막히지 않게 한다.
- 링크 시간 최적화(계획 27.5), hot patch.
- C++20 modules와 header units. UE에서도 실험 단계다(`Configuration/Rules/TargetRules.cs:1840, 2482`). PCH 생성·사용을 툴체인별 함수로 감싸 나중에 바꿀 자리만 둔다.

## 4. 선행: 정밀 헤더 의존(UB0)

1절의 헤더 의존 방식에서는 헤더 하나의 변경이 include 디렉터리를 공유하는 모든 TU를 무효화한다. 이 상태로 PCH를 넣으면 PCH와 모든 소비 TU가 함께 무효화되어 이득이 줄고, 작업 파일 분리도 헤더 편집에는 효과가 없다. 그래서 유니티·PCH보다 먼저 다음을 둔다. 이 단계만으로도 I4의 재컴파일 범위가 줄어든다.

### 4.1 2단 key

1. **선언 입력 key**: 이전 실행 결과에 의존하지 않는다. 다음을 담는다.
   - 툴체인 key
   - action 인자
   - TU 구성(4.2절)
   - 구성원 소스 내용
   - 정의 헤더 내용
   - 소비하는 PCH의 기록 해시
   - 선언 추가 입력
   - include 디렉터리와 소스 디렉터리의 **헤더 이름 목록**(존재 집합)

   이름 목록은 새 shadow header, `__has_include` 결과 변화, 파일 삭제를 잡는다(계획 27.2 "negative include 변화도 재검사", ccache direct mode 반례).
2. **의존 기록 검증**: 선언 입력 key가 기록과 같으면 지난 실행의 의존 기록(연 헤더 목록과 각 내용 해시)을 다시 확인한다. 모든 헤더의 해시가 같아야 hit다. 하나라도 다르거나, 기록이 없거나, 깨졌으면 다시 실행한다.

성질과 처리 규칙:
- 같은 파일 상태에서는 판정이 같다. 기록은 믿는 값이 아니라 검증하는 값이다. 원격 캐시도 같은 구조(선언 key로 찾고 의존 기록으로 검증)를 쓸 수 있다.
- 헤더 내용 전체 해시는 하지 않는다.
- **툴체인 소유 헤더**: 아래 위치의 헤더는 action마다 해시하지 않는다. 툴체인 key(컴파일러 digest, keyed 환경, `FingerprintPaths`)가 대신한다.
  - 툴체인 `OwnedRoots`
  - `FingerprintPaths` 아래
  - keyed 환경 `INCLUDE`가 가리키는 SDK·CRT 디렉터리
- **depfile 철자**: clang++는 `-MD -MF <파일>`이고, clang-cl은 `/clang:-MD /clang:-MF<파일>`이다. 둘 다 실측으로 동작을 확인했다.
- **depfile 파서**는 Make 문법을 따른다.
  - 공백은 `\ `로 escape된다.
  - Windows의 역슬래시 구분자와 escape를 구별한다.
  - 줄 끝 `\`는 줄 이음이다.
  - 상대 경로(작업 디렉터리 = 루트 기준)와 절대 경로가 섞여 나온다.

  파서는 공백·한글·괄호가 든 경로를 fixture로 시험한다.
- **의존 기록 게시**: 의존 기록은 action 출력과 함께 원자적으로 게시한다. 실패하거나 취소된 실행은 기록을 남기지 않는다.
- PCH를 쓰는 TU의 depfile에는 PCH 입력 헤더가 들어가지만 `.pch` 자체는 들어가지 않는다. `.pch`는 선언 입력(PCH action의 기록 해시)으로 key에 들어간다.
- UE는 의존 목록을 쓰지만 판정은 타임스탬프로 하고 내용 해시를 쓰지 않는다(`Actions/ActionGraph.cs:706–822`, `System/CppDependencyCache.cs`). 산출물보다 1초 넘게 새로울 때만 낡은 것으로 본다. Rocks는 내용 해시를 쓰므로 브랜치 전환이나 touch로 헛 재빌드가 일어나지 않는다. 시간 오차 허용은 두지 않는다.

### 4.2 compile identity 정리

compile action identity에서 모듈 전체 소스 목록을 빼고 그 action의 TU 구성만 넣는다. 단독 TU면 소스 하나, 묶음이면 구성원 목록이다(6절). 전체 목록을 넣은 원래 이유(예: "source 옆 local header" 보수 추적)는 UB0 착수 때 확인한다. 필요한 성질은 4.1의 이름 목록으로 대신한다.

### 4.3 큰 산출물 검증 비용

- 현재 cache hit은 모든 출력의 실제 SHA-256이 일치해야 한다(README "입력과 증분 계약"). PCH는 수십–수백 MB라서 무변경 빌드(I0 ≤2 s)마다 다시 해시하면 목표를 넘는다.
- 출력 검증은 (크기, 수정 시각, 파일 ID)가 기록과 모두 같으면 기록된 해시를 쓰고, 하나라도 다르면 다시 해시한다. 시각만으로 hit를 판정하는 것은 아니다. 기록된 해시는 key와 묶여 있고, 메타데이터가 바뀌면 내용을 다시 본다.
- 같은 plan 안의 산출물(PCH·archive)을 입력으로 받는 action은 그 산출물의 기록 해시를 입력 해시로 쓰고, 파일을 다시 읽지 않는다.

### 4.4 경로 철자

**문제**
- Windows와 macOS(기본 APFS)의 파일 시스템은 대소문자를 구분하지 않고 보존한다. 같은 디렉터리를 `c:\work\rocks`로도 `C:\Work\Rocks`로도 열 수 있다.
- 빌드는 받은 철자를 그대로 인자(include 디렉터리·소스 경로)와 action key에 넣었다. 그래서 같은 트리를 철자만 다르게 실행하면(작업 디렉터리의 드라이브 문자가 소문자인 셸, 다른 대소문자를 넘기는 스크립트) 모든 action이 다시 실행되고 `compile_commands.json`의 경로도 달라졌다.

**규칙**: 빌드는 받은 경로를 디스크에 있는 철자로 바꿔서 쓴다(`Rocks.Build/PathSpelling.cs`의 `PathSpelling.OnDisk`).
- 드라이브 문자는 대문자로 한다.
- 루트부터 성분마다, 그 이름의 항목이 부모 디렉터리에 있으면 파일 시스템이 알려 주는 이름으로 바꾼다. 없는 성분부터는 받은 그대로 둔다(아직 만들지 않은 출력 디렉터리).
- 링크는 풀지 않는다. symlink·junction·`subst` 드라이브를 거친 경로도 받은 성분의 이름만 바로잡는다.
- 대소문자를 구분하는 파일 시스템에서는 정확히 같은 이름을 먼저 찾으므로 받은 철자가 바뀌지 않는다.

**적용 지점**
- `NativeBuildEngine.BuildAsync`가 요청을 받자마자 한 번 한다. 대상은 `RootDirectory`·`OutputDirectory`·`SdkDirectory`, 모듈의 경로 목록(`Sources`·`ReflectionSources`·include 디렉터리·`PrebuiltLibraries`·`AdditionalInputs`·`UnityExclude`·`PrivatePchHeader`), 툴체인의 compiler·archiver·linker 경로다. 상대 경로는 루트 기준으로 푼 뒤 바꾼다. 그 뒤의 단계(모듈 표, 생성 소스, BuildInfo, 액션 그래프, compile DB, 실행기)는 모두 바꾼 요청을 본다.
- CLI는 `--root`·`--output`·`--sdk`·`--project`·`--destination`·`--compiler`를 읽을 때 한다. 규칙 평가와 메타 생성이 빌드 엔진보다 먼저 이 경로로 파일을 쓰기 때문이다.
- 그래서 action key, 의존 기록, `compile_commands.json`, 도구 빌드 ID가 실행할 때 쓴 철자와 무관하다.

**근거와 버린 대안**
- UE도 사용자가 준 경로를 성분마다 디스크의 철자로 바로잡는다. `EpicGames.Core/DirectoryUtils.cs:18`의 `FindCorrectCase`가 부모 디렉터리를 열거해 이름을 맞추고 드라이브 문자를 대문자로 하며, 없는 경로는 그대로 둔다. UBT는 프로젝트 파일 경로에 쓴다(`UnrealBuildTool/Configuration/Descriptors/TargetDescriptor.cs:509`).
- 최종 경로(`GetFinalPathNameByHandle`)는 쓰지 않는다. 철자뿐 아니라 symlink·junction·`subst`를 풀어 다른 경로를 돌려준다. 출력이 루트 아래인지 보는 검사와, 다른 루트·`subst`·junction에서도 같은 도구 빌드 ID(8.7절)가 받은 경로의 모양에 기댄다.
- key를 계산할 때만 대소문자를 접는 방법도 쓰지 않는다. 인자와 compile DB, 진단에 찍히는 경로는 여전히 실행마다 달라지고, 대소문자를 구분하는 파일 시스템에서는 서로 다른 파일을 같게 본다.

**한계**
- 있는 경로의 8.3 짧은 이름(`PROGRA~1`)은 긴 이름이 된다(.NET `Path.GetFullPath`가 편다). 아직 없는 경로의 짧은 이름은 받은 그대로다.
- 환경 변수(`INCLUDE`·`LIB`)에 적힌 경로의 철자는 바꾸지 않는다.
- 비용은 성분마다 디렉터리 조회 한 번이고, 같은 디렉터리는 요청 하나에서 한 번만 조회한다.

## 5. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 묶음 경계 | 경로 해시 anchor로 구간을 나누고(고정 간격), 구간이 상한을 넘으면 구간 안을 파일 수로 균등하게 나눈다. 크기는 실제 바이트다(6.2절) | UE는 정렬 후 크기를 누적해 끊는다(`System/Unity.cs:116–124`). 앞 파일이 커지거나 하나가 추가되면 뒤 묶음 경계가 모두 밀린다 |
| 묶음 크기 | 상한 384 KiB, 구간 기대 파일 수 16. 둘 다 모듈별로 재정의할 수 있다 | UE 기본 `NumIncludedBytesPerUnityCPP = 384 * 1024`(`TargetRules.cs:2028`) |
| 묶지 않는 것 | BuildInfo·규칙 생성 TU, C 소스, 외부 라이브러리 모듈, 첫 include 앞에서 `#define`하는 파일 | 생성 시점이 다르거나, 언어가 다르거나, 묶으면 설정이 조용히 무효가 된다 |
| 묶음 include 철자 | 루트 기준 경로 `#include "Core/Source/…"`. 묶음 TU에만 `/I<root>`를 맨 뒤에 붙인다 | 단독 TU와 같은 `__FILE__`이 나온다(실측). 출력 폴더 이름이 섞이지 않는다 |
| 작업 파일 판별 | git(`status --porcelain`)만 쓴다. timeout 없이 끝날 때까지 기다리고, 실패는 오류다. CI·`--adaptive off`·git 없는 트리는 빈 집합이다 | UE는 git 15초 timeout과 Perforce read-only 판별을 쓴다(`System/SourceFileWorkingSet.cs:69–165`) |
| 분리 파일 자리 | 경계는 작업 파일을 포함한 전체 목록으로 계산한다. 분리는 그 뒤 구성원에서만 뺀다 | UE는 빠진 자리를 가상 크기로 예약한다(`Unity.cs:275`). 같은 목적이다 |
| PCH 종류 | 모듈 PCH와 공유 PCH. 기본은 "모듈 PCH가 있으면 그것, 없으면 공유 PCH" | UE `UseExplicitOrSharedPCHs`(`Configuration/Rules/ModuleRules.cs:195–221, 816–846`) |
| PCH 사용 방식 | 소스는 PCH를 직접 include하지 않는다. 빌드가 강제 include한다 | UE와 같다. 소스는 PCH 없이도 컴파일되어야 한다 |
| 정의 | 모듈 정의는 명령줄에서 생성 정의 헤더로 옮긴다. 구성·플랫폼·타깃 매크로는 명령줄에 남긴다 | UE `CreateHeaderForDefinitions`(`Configuration/UEBuildModuleCPP.cs:2322`). 정의가 같은 TU끼리만 PCH를 공유할 수 있다 |
| 강제 include 순서 | [PCH 앞부분: 정의 헤더] → [PCH 헤더] → [소비 추가 정의 헤더]. PCH 앞부분은 PCH 생성과 바이트까지 같고 변형 key에 들어간다 | clang-cl `/Yu`는 through header 앞의 `/FI`를 검증 없이 건너뛴다(실측) |
| PCH 변형 | 경로가 아닌 compile 인자와 PCH 앞부분의 해시로 자동 구분한다. include 경로는 key에서 빼고 포함 관계와 shadow를 따로 검사한다 | UE는 축을 코드로 나열한다(`IsCompatibleForSharedPCH`, `UEBuildModuleCPP.cs:1798–2014`). 축을 빠뜨리면 잘못된 PCH를 쓴다 |
| 추가 정의 검사 | 소비 모듈의 추가 정의가 공유 PCH 헤더 closure의 전처리 조건에 쓰이면(매크로 본문을 거친 간접 참조 포함) 빌드 오류다 | UE는 검사하지 않는다. 정의 헤더로 옮기면 clang도 경고하지 않는다(실측: PCH 0, 비PCH 1) |
| 묶음 충돌 | 같은 모듈에서 파일 지역 이름(익명 namespace·`static`)이 겹치면 정책 오류다. 묶음 구성과 무관하게 모듈 단위로 본다 | UE는 규약에만 맡긴다. 겹치는 이름은 묶음 안에서 조용히 다른 함수를 고를 수 있다(9.1절) |
| 검증 빌드 | 유니티·PCH를 끈 빌드를 별도 object 변형으로 두고 CI에서 유지한다 | 계획 27.3, UE `NU`·NoPCH intermediate(`Configuration/UEBuildTarget.cs:1184–1219`) |
| compile DB·도구 빌드 ID | 원본 소스 단위의 논리 명령으로 만든다 | 유니티·PCH·작업 파일 분리가 ID와 clangd 입력을 바꾸지 않는다 |
| 측정 | `-ftime-trace` 집계와 묶음 크기 실측 모드 | UE `-Timing`(`Platform/Windows/VCToolChain.cs:2141–2150, 2644–2700`), `-Mode=ProfileUnitySizes`(`Modes/ProfileUnitySizesMode.cs:182–240`) |

## 6. 유니티 빌드

### 6.1 묶을 대상

모듈의 C++ 소스(`NativeLanguage.Cpp`) 중 다음을 뺀 나머지를 묶는다.
- C 소스(`.c`). 언어가 다르다.
- BuildInfo TU와 규칙 생성 TU(`GeneratedSources`). 생성 시점과 빈도가 다르다.
- 규칙이 단독으로 지정한 파일(`UnityExclude`). 큰 파일이나 특수 플래그 파일이다. **뒤 소스로 새는 매크로를 정의하는 헤더를 include하는데 감쌀 수 없는 파일**도 여기다(Windows SDK 헤더는 예외 없이 감싼다 — 아래). Windows COM 헤더는 `small`·`interface` 같은 매크로를 남기고, 묶음에서 그 파일 뒤에 오는 소스가 묶음 경계에 따라 달라지므로, 상관없는 소스를 더하기만 해도 빌드가 깨진다(2026-11-20, rocks-73이 EngineTests 묶음 경계가 바뀌며 발견 — 그때는 `TestSupport/Private/Win64/WindowsComRefTests.cpp`를 단독으로 뺐다). 이 누출은 아래 "매크로를 남기는 헤더"의 규칙과 정책 검사가 막는다. 그 규칙이 생기면서 `WindowsComRefTests.cpp`는 묶음으로 돌아왔다.
- **첫 `#include` 앞에서 `#define`하는 파일**(예: `Core/Tests/StlCompatibilityTests.cpp`의 `_HAS_EXCEPTIONS 0`). 같은 묶음의 앞 파일이나 PCH가 그 헤더를 이미 처리했으면 설정이 조용히 무효가 된다. 빌드가 토큰 단위로 찾아내 단독 TU로 컴파일하고 PCH도 끈다. 빌드 보고에 파일 목록을 남긴다.
- 상한보다 큰 파일. 단독 TU다.

모듈 규칙 `Unity`가 `Off`면 묶지 않는다. 외부 라이브러리 모듈(`Core/Source/ThirdParty`)의 기본은 `Off`다. upstream이 유니티를 전제하지 않는다.

**매크로를 남기는 헤더**
- Windows SDK 헤더는 평범한 이름의 매크로를 번역 단위 끝까지 남긴다. `minwindef.h`의 `near`·`far`·`pascal`·`cdecl`, `rpcndr.h`의 `small`·`hyper`, `combaseapi.h`의 `interface`, 그리고 `CreateDirectory`·`GetObject`·`RegisterClass` 같은 A/W 함수 이름 매크로다. 묶음에서는 이 매크로가 그 파일 뒤의 구성원까지 살아남아, 같은 이름을 변수·멤버·함수로 쓰는 소스를 깨뜨린다. 어느 소스가 뒤에 오는지는 묶음 경계에 달려 있다.
- 규칙: Windows에서 빌드하는 모든 모듈의 소스, 그 소스가 include하는 저장소 헤더, 모듈 PCH 헤더는 Windows SDK 헤더를 **감싸서** include한다. 외부 라이브러리 원본(`ThirdParty`)만 대상이 아니다.
  ```cpp
  #include <Platform/Win64/WindowsHeadersBegin.h>

  #include <d3d11_4.h>
  #include <dxgi1_6.h>

  #include <Platform/Win64/WindowsHeadersEnd.h>
  ```
  - 유니티를 끈 모듈과 `UnityExclude`의 소스도 대상이다. 쌍이 매크로를 숨긴 뒤에는 같은 번역 단위에서 뒤에 여는 SDK 헤더도 쌍 안이어야 파싱되기 때문이다. `Platform/Win64/ComRef.h`처럼 쌍으로 끝나는 헤더를 include한 소스가 그 뒤에 `<d3d12.h>`를 쌍 밖에서 열면 `interface`가 없어 깨진다. 묶음과 상관없는 일이므로 예외를 두지 않는다.
  - `WindowsHeadersEnd.h`는 아래 이름을 `#undef`한다. 상태를 저장하지 않는다.
    - 평범한 이름: `near`·`far`·`small`·`interface`·`pascal`·`cdecl`·`hyper`·`min`·`max`. `FAR`·`NEAR`는 빈 매크로로 바꾼다. `DEFINE_GUID`나 `FD_SET` 같은 SDK 매크로는 쓰는 자리에서 `FAR`로 펼쳐지는데, 원래 정의(`#define FAR far`)는 `far`를 지운 뒤에는 컴파일되지 않는다.
    - A/W 함수 이름: UE가 `Windows/PostWindowsApi.h`에서 지우는 A/W 이름 33개와, 엔진 식별자와 실제로 겹친 `RegisterClass`·`FindFirstFile`이다.
  - `WindowsHeadersBegin.h`는 `Windows.h`를 열고 End를 거친 뒤(A/W 이름을 지운다) 평범한 이름을 SDK와 같은 정의로 다시 `#define`한다(`near`·`far`·`cdecl`은 빈 매크로, `pascal`은 `__stdcall`, `small`은 `char`, `hyper`는 `__int64`, `interface`는 `__STRUCT__`, `FAR`·`NEAR`는 `far`·`near`). 그래서 감싼 구간의 헤더는 언제나 A/W 이름 매크로가 없고 평범한 이름 매크로는 있는 상태로 파싱된다.
    - `ID3D12InfoQueue::GetMessage`나 `ID2D1RenderTarget::DrawText` 같은 COM 메서드가 include 순서와 상관없이 제 이름으로 선언된다.
    - Windows 헤더는 include guard 때문에 `interface`나 `far`를 다시 정의하지 않는데, 뒤에 처음 include되는 COM·DirectX 헤더는 그것이 있어야 파싱된다. SDK 헤더가 같은 이름을 다시 정의해도 토큰이 같아 경고가 없다.
    - 구간에 `Windows.h`를 따로 적지 않아도 된다(적어도 해가 없다).
    - `min`·`max`는 되돌리지 않는다. 빌드가 `NOMINMAX`를 전역으로 정의하므로 처음부터 없다. End의 `#undef`는 안전장치다.
  - 두 헤더는 `#pragma push_macro`/`pop_macro`를 쓰지 않는다. clang의 PCH는 push 스택을 저장하지 않아서, PCH 안에서 End가 저장한 것을 PCH를 쓰는 번역 단위의 Begin이 되돌리지 못한다(경고 뒤 `interface`가 없어 SDK 헤더가 깨진다). 정의를 직접 다시 쓰면 번역 단위나 PCH 경계를 넘는 상태가 없다. 쌍은 PCH 헤더 안에 있어도 되고 PCH를 쓰는 소스에 있어도 된다.
  - 두 헤더는 `#pragma once` 없이 여러 번 include하는 조각이다. `Platform/Win64/`에 있고 Windows native backend 소스만 include한다(정책 `platform-boundary`). 그 정책은 `Platform/Win64/` 헤더의 틀(`#pragma once`, `#include <Base/Compiler.h>`, `#if ROCKS_PLATFORM_WINDOWS` … `#endif`)에서 이 둘만 `#pragma once`를 빼고 받는다. 이 둘에 `#pragma once`가 있으면 어느 줄이든 거부한다(두 번째 include가 비어 버린다).
  - 감싼 구간 안에는 Windows SDK 헤더와, SDK 헤더를 여는 외부 라이브러리 헤더만 둔다(저장소 헤더를 넣으면 그 헤더가 매크로가 살아 있는 상태로 파싱된다).
  - 감싼 뒤의 코드는 숨긴 매크로를 쓸 수 없다. A/W 함수는 `W` 이름을 직접 쓴다(`CreateDirectoryW`). `Windows.h` 자신이 선언하는 struct의 멤버 이름이 함수 이름 매크로와 겹치면(`FILE_DISPOSITION_INFO::DeleteFile`은 선언될 때 `DeleteFileA`/`DeleteFileW`가 된다) 이름 없이 초기화한다(`FILE_DISPOSITION_INFO{TRUE}`).
- 정책 검사 `windows-macro-leak`(9.2절)이 쌍 밖의 include를 거부한다. Windows 플랫폼의 모듈 규칙만 평가한다(다른 플랫폼의 소스는 Windows 헤더를 열지 않는다).
  - 읽는 범위: 모듈의 C++ 소스와 PCH 헤더, 거기서 include로 닿는 저장소 헤더다. 줄 이음(`\`)을 먼저 잇고, 전처리 조건은 평가하지 않고 두 갈래를 모두 읽는다.
  - 쌍 밖에 둘 수 있는 include는 셋뿐이다: 저장소 헤더, 생성 헤더(`*.generated.h`), **깨끗한 시스템 헤더 목록**에 있는 헤더(C·C++ 표준 헤더, CRT, 컴파일러 intrinsic 헤더 — `Rocks.Tooling.Checks`의 목록). 그 밖의 include는 Windows SDK 헤더로 보고 쌍 안에 있어야 한다. 처음 쓰는 SDK 헤더는 이름을 몰라도 걸린다. 매크로를 남기지 않는 새 시스템 헤더는 목록에 더한다.
  - 외부 라이브러리 헤더(저장소의 `ThirdParty`)는 따라가서 읽는다. 그 헤더나 그 헤더가 include하는 외부 라이브러리 헤더가 알려진 Windows SDK 헤더 이름(`windows.h`, `winsock2.h`, `unknwn.h`, `d3d*.h` 등)을 include하면, 저장소 소스가 그 외부 헤더를 include하는 줄이 쌍 안에 있어야 한다. 전처리 조건을 평가하지 않으므로, SDK include가 저장소가 정의하지 않는 매크로의 갈래에만 있다고 확인한 헤더는 정책의 목록에 경로로 적어 봐준다(`zconf.h`의 `ZLIB_WINAPI` 갈래, `vulkan/vulkan.h`의 `VK_USE_PLATFORM_WIN32_KHR` 갈래). 경로에 버전이 들어 있어, 라이브러리를 올리면 다시 걸리고 그때 갈래를 다시 확인한다.
  - 꺾쇠나 따옴표가 아닌 include(`#include HEADER_MACRO`)는 판정할 수 없으므로 쌍 밖에서는 거부한다.
  - 쌍이 어긋난 것(Begin 없는 End, 닫지 않은 Begin, 이미 연 Begin)과 쌍 안의 저장소 헤더도 거부한다. 쌍은 파일마다 스스로 닫아야 한다.
- UE 대응: UE는 `Windows/WindowsHWrapper.h`(Pre·MinWindows·Post)로만 `windows.h`를 열어 A/W 이름 매크로를 바로 지우고, 다른 Windows 헤더는 `AllowWindowsPlatformTypes.h`/`HideWindowsPlatformTypes.h` 쌍으로 감싼다.
  - Rocks는 쌍 하나로 둘을 겸한다(Begin이 `Windows.h`를 열고 지우는 순서가 같다). 감싸지 않은 include는 정책 검사가 거부한다(UE는 관례다).
  - UE의 `push_macro`/`pop_macro`는 Pre/Post 한 쌍 안에서 끝난다(`PreWindowsApi.h`가 저장한 `TEXT`·`TRUE`·`FALSE` 등을 `PostWindowsApi.h`가 되돌린다). `WindowsHWrapper.h` 한 헤더 안에서 열고 닫으므로 PCH 경계를 넘지 않는다. Rocks가 처음 쓴 방식은 End가 저장하고 다음 Begin이 되돌려서 경계를 넘었다.
  - `PostWindowsApi.h`는 A/W 이름 말고도 `INT`·`UINT`·`DWORD`·`FLOAT`와 `InterlockedIncrement` 같은 atomics 매크로도 지운다. Rocks는 A/W 이름만 따른다. UE는 같은 이름의 자기 타입과 함수가 있어서 지우는데, Rocks에는 그 이름의 식별자가 없고(정수 타입은 `i32`·`u32`, atomics는 `Platform/Atomics.h`), Windows backend 소스는 쌍 뒤에서 SDK의 `DWORD`·`InterlockedIncrement`를 그대로 쓴다.
  - UE 5.8은 `near`·`far`·`small`·`interface`를 남긴다(그 이름을 식별자로 쓰지 않는 관례로 피한다). Rocks는 그것들도 숨긴다.
- 버린 대안:
  - End가 `#pragma push_macro`로 저장하고 다음 Begin이 `pop_macro`로 되돌리는 방식(처음 구현). 저장한 스택이 PCH 경계를 넘지 못한다(위). A/W 이름까지 되돌리면 `Windows.h`와 같은 구간에서 파싱된 COM 헤더는 메서드를 `GetMessageA`로 선언하고 뒤 구간에서 파싱된 헤더는 `GetMessage`로 선언해서, 같은 호출이 include 순서와 묶음 구성에 따라 컴파일되거나 깨진다.
  - 정책이 누출 헤더를 이름 목록으로 고르는 방식(처음 구현). `fileapi.h`·`rpcndr.h`·`dxcore.h`·`xaudio2.h`·`commctrl.h`처럼 목록에 없는 SDK 헤더가 통과한다. 깨끗한 헤더 목록으로 뒤집으면 모르는 헤더가 걸린다.
  - include가 툴체인의 Windows Kits 디렉터리로 해석되는지로 판정하는 방식. 정책 검사는 Linux·macOS CI에서도 도는데 그 기기에는 Windows SDK가 없어, 같은 위반이 기기에 따라 통과한다.
  - 유니티를 켠 모듈의 묶이는 소스만 검사하는 방식(처음 구현). 유니티를 끈 모듈(`ShaderToolSpike`)이 쌍으로 끝나는 `ComRef.h` 뒤에 `<d3d12.h>`를 쌍 밖에서 열어 빌드가 깨졌고, CI가 빌드하지 않는 모듈이라 드러나지 않았다.
  - 빌드가 묶음 구성원 사이에 `#ifdef near` → `#error` 검사를 끼우는 방식. 묶음일 때만 잡고, 단독 번역 단위 검증 빌드와 정책 검사 단계에서는 보이지 않는다.
  - Windows 소스를 모두 `UnityExclude`에 두는 방식. Windows backend 모듈(D3D11·D3D12·PlatformBase)은 소스 대부분이 Windows 헤더를 써서 묶음 이득을 잃고, 숨긴 뒤의 include 문제는 그대로다.
- 한계:
  - 숨기는 이름은 고정 목록이다. 목록에 없는 A/W 매크로(`CreateFile`, `CreateWindow`, `GetModuleHandle` 등 수백 개)와 대문자 매크로(`DELETE`, `ERROR`, `IN`, `OUT`), `uuid_t`·`lstrlen` 같은 소문자 매크로는 End 뒤에도 남는다. 엔진 식별자와 겹치는 것이 나오면 End의 목록에 더한다. 2026-11-21에 SDK 10.0.22621의 매크로 덤프(쌍 뒤에 남는 이름 중 소문자가 섞인 1,733개)를 저장소 소스의 식별자와 대조해 겹치는 셋(`pascal`·`RegisterClass`·`FindFirstFile`)을 더했다.
  - 검사는 include 줄의 철자와 순서만 본다. 전처리 조건을 평가하지 않으므로, 외부 라이브러리 헤더가 쓰이지 않는 갈래에서만 `windows.h`를 열어도 걸린다(확인한 뒤 위 목록에 적는다). 외부 라이브러리 헤더 안의 SDK include는 알려진 이름으로만 알아보므로, 낯선 SDK 헤더만 여는 외부 헤더는 놓친다.
  - C 소스(`.c`)는 보지 않는다. 묶지 않고, 쌍은 C++ 헤더(`Base/Compiler.h`)를 거친다.
- 시험:
  - 쌍: native 시험 `TestSupport/Private/Win64/WindowsHeadersTests.cpp`(숨긴 이름을 식별자로 쓰기, A/W 매크로가 지워졌는지, COM 메서드가 제 이름인지)와 빌드 시험 `Rocks.Build.Tests`의 `windows-headers` 묶음(쌍으로 끝나는 PCH 헤더를 `/Yc`·`/Yu`로 쓰는 소스, PCH를 끄고 `/FI`로 받는 변형, `Windows.h`가 쌍보다 먼저 쌍 밖에서 열린 소스).
  - 정책: `Policy.WindowsMacroLeaksRejectIncludesOutsideThePair`(양성·음성 대조)와 `Policy.WindowsPublicHeadersStayNarrow`(쌍 헤더의 틀).

UB1 동안 모든 모듈의 기본은 `Off`다. UB2·UB3에서 충돌을 정리한 모듈부터 켠다(9.3절).

### 6.2 경계 결정(결정적)

작업 파일 분리(7절) 전, 6.1의 대상 목록 **전체**로 계산한다.

1. 대상 파일을 저장소 상대 경로(`/` 구분, 대소문자 보존)의 ordinal 순서로 정렬한다.
2. 각 파일의 경로 해시(SHA-256 앞 8 byte)를 구한다. 해시를 `UnityAnchorFiles`(기본 16, 모듈별 재정의)로 나눈 나머지가 0인 파일 **앞**에서 새 구간을 시작한다. 간격은 고정값이고 파일 크기와 무관하다.
3. 구간의 실제 바이트 합이 상한(`UnityBatchBytes`, 기본 384 KiB)을 넘으면, 구간을 k = ⌈합 ÷ 상한⌉개로 **파일 수 기준 균등 분할**한다. 앞쪽 몫이 하나 많다. 크기가 늘어도 k가 바뀌기 전까지 자르는 자리는 그대로다.
4. 구성원이 하나뿐인 묶음은 묶음 파일 없이 단독 TU로 컴파일한다.

이렇게 정한 성질:
- **국소성**: 파일 하나를 추가·삭제·이름 변경·편집해도 그 파일이 속한 구간만 바뀐다. 추가하거나 지운 파일이 anchor면 앞 구간과 합쳐지거나 나뉘어 두 구간이 바뀐다. 다른 구간은 그대로다.
- **크기**: 묶음 크기는 대략 기대 파일 수 × 평균 크기다. 현재 소스(평균 12–16 KiB) 기준 약 190–250 KiB이고 상한을 넘지 않는다. `UnityAnchorFiles`와 상한은 `unity-profile`(12절) 실측으로만 바꾼다.
- **재구성**: 두 값은 묶음 구성의 입력이다. 모듈별 재정의를 바꾸면 그 모듈의 묶음이 다시 구성되는데, 이는 정상 동작이다.

### 6.3 묶음 파일

- **위치**: `<out>/Unity/<Module>/<Module>.Unity.<id>.cpp`. `<id>`는 묶음 첫 구성원 경로 해시의 앞 12자리 hex다. 순번이 아니라서 앞 묶음이 생기거나 사라져도 이름이 밀리지 않는다.
- **내용**: 머리말 한 줄(`// Generated by Rocks.Build. Do not edit.`)과 구성원마다 `#include "<루트 기준 경로>"`(`/` 구분)를 쓴다.
- **include 탐색**: 묶음 TU에만 `/I<root>`(`-I<root>`)를 include 목록 **맨 뒤**에 붙인다.
  - 구성원 안의 `#include "..."`는 그 구성원 파일의 디렉터리부터 찾고, 그다음 기존 include 목록 순서를 따른다. 맨 뒤 루트는 다른 곳에서 못 찾는 철자에만 걸린다.
  - 그런 철자는 단독 TU에서 오류이므로 검증 빌드가 잡는다.
  - `<root>`는 요청 루트다. 설치 SDK 게임 빌드에서는 게임 루트이고 엔진 모듈은 prebuilt라서 묶지 않는다.
- **`__FILE__`과 경로 매핑**: 기존 `-fmacro-prefix-map=<root>=`가 적용되어 묶음 구성원의 `__FILE__`은 단독 TU와 같은 저장소 상대 경로가 된다. 실측: 상대 경로 include는 `out\Unity\M\..\..\..\src\a.cpp`, 루트 기준 include는 `src\a.cpp`였다. 묶음 내용은 호스트에 무관하다.
- **쓰기**: 출력 잠금 아래에서 `AtomicFile.WriteIfChanged`로 쓴다. 구성이 같으면 다시 쓰지 않는다.
- **진단**: 오류 위치는 원본 파일 경로로 나온다. `#include`라서 `#line`이 필요 없다.

### 6.4 action·object·정리

- 묶음 하나 = compile action 하나다. 입력은 묶음 파일과 구성원 소스 전부이고, identity는 구성원 목록이다(4.2절).
- object 경로는 `obj/<Module>/<변형>/<묶음 또는 소스 id>.obj`다. `<변형>`은 유니티·PCH 모드 조합(`u1p1`·`u0p0` 등)이다. 검증 빌드와 일반 빌드를 번갈아 돌려도 서로의 object를 덮어쓰지 않는다.
- 빌드가 성공하면 **이번 변형** 디렉터리(`obj/<Module>/<변형>/`, `Unity/<Module>/`)에서 이번 plan이 만들지 않은 파일을 지운다. 그대로 두면 낡은 묶음이 검사 도구와 디스크에 남는다. 다른 변형 디렉터리는 건드리지 않는다.

## 7. 작업 파일 분리

### 7.1 작업 파일 집합

- 모듈 소스가 git 작업 사본 안이면 그 작업 사본 루트에서 `git --no-optional-locks status --porcelain=v1 -z --untracked-files=all`을 실행한다. 빌드 한 번에 저장소마다 한 번이다.
  - 작업 사본 루트는 빌드 루트에서 올라갈 상대 경로(`git rev-parse --show-cdup`)로 정하고, 보고된 상대 경로를 그 루트에 붙여 소스와 같은 철자의 절대 경로를 만든다. `--show-toplevel`은 symlink·junction을 푼 실제 경로를 돌려주므로(macOS 임시 폴더 `/var` → `/private/var`, Windows junction) 소스 철자와 갈려 작업 파일이 하나도 잡히지 않는다(macOS CI에서 발견, 2026-11-20).
- 각 항목의 상태 두 열(X·Y) 중 하나라도 공백이 아니면 작업 파일이다. 수정(`M`), 추가(`A`), 형식 변경(`T`), 충돌(`U`), 미추적(`??`)을 모두 포함한다. 이름 변경(`R`)과 복사(`C`)는 새 경로를 작업 파일로 본다.
- **timeout을 두지 않는다.** 결과가 나올 때까지 기다리고, 실패(종료 코드 ≠ 0)면 빌드 오류로 알린다. git 상태가 정해지지 않은 채 묶음을 구성하면 같은 입력에서 다른 action이 나온다. UE는 15초 timeout 뒤 빈 집합으로 진행한다(`System/SourceFileWorkingSet.cs:118–165`).
- 다음 경우는 빈 집합이다(분리 없음).
  - `--adaptive off`
  - CI 실행. 환경으로 추측하지 않고 CI 명령이 `--adaptive off`를 명시한다.
  - git 작업 사본이 아닌 트리(설치 SDK 게임 프로젝트 등). 정보 로그를 1회 남긴다.

### 7.2 분리 규칙

- 다음 `.cpp`를 묶음 구성원에서 빼서 단독 TU로 컴파일한다.
  - 자신이 작업 파일인 `.cpp`
  - 짝 헤더(첫 include, 10절)가 작업 파일이고, 그 **헤더 이름**이 모듈 안에서 유일한 `.cpp`. UE와 같다(`System/Unity.cs:374–406`).
- 경계(6.2절)는 분리 전 전체 목록으로 정해져 있으므로, 분리는 그 파일이 속한 묶음 하나만 바꾼다.
- 분리된 파일은 모듈의 PCH를 그대로 쓴다(기본). 규칙 `AdaptivePch = Off`면 PCH 없이 컴파일한다.
- 파일을 처음 편집하면 그 파일이 빠진 묶음이 한 번 다시 컴파일되고, 이후 편집은 그 파일만 컴파일한다. 커밋해서 작업 파일에서 빠지면 묶음으로 돌아가며 그 묶음이 한 번 다시 컴파일된다(UE와 같다).
- 작업 파일 헤더마다 단독 헤더 컴파일 검사를 붙이는 기능(UE `bAdaptiveUnityCompilesHeaderFiles`)은 두지 않는다. 기존 공개 헤더 검사(`CheckPublicHeaders`)가 맡는다.

## 8. PCH

### 8.1 모델

| 모드(`PchUsage`) | 동작 |
|---|---|
| `Auto`(기본) | 모듈 PCH(`PrivatePchHeader`)가 있으면 그것을, 없으면 사용할 수 있는 공유 PCH(8.3절)를 쓴다. 둘 다 없으면 PCH 없음 |
| `PrivateOnly` | 모듈 PCH만 쓴다 |
| `Off` | PCH를 쓰지 않는다. 외부 라이브러리 모듈의 기본이다 |

- 소스는 PCH 헤더를 `#include`하지 않는다. 빌드가 강제 include(`/FI`·`-include`)한다. 소스는 PCH가 없어도 컴파일되어야 하고, 검증 빌드(11절)가 이를 확인한다.
- PCH는 C++ TU에만 쓴다.
- 모듈 PCH는 PCH를 쓸 TU(묶음 + 단독, 6.1의 제외 파일 제외)가 `PrivatePchMinTus`(기본 3)개 이상일 때만 만든다. 그보다 적으면 PCH를 만드는 비용이 절약분보다 크다. UE `MinFilesUsingPrecompiledHeader`(`UEBuildModuleCPP.cs:672`)는 계산만 하고 쓰지 않는 값이라 대응 규칙을 새로 둔다. 공유 PCH는 한 번 만들어 여러 모듈이 쓰므로 이 하한을 적용하지 않는다.

### 8.2 정의 헤더

- 모듈 정의(전파된 dependency public define과 모듈 public/private define)를 명령줄에서 빼서 `<out>/Definitions/<Module>.Definitions.h`에 쓴다. 형식은 `#define NAME value` 줄을 정렬한 것이고 `WriteIfChanged`로 쓴다.
- 구성·플랫폼·타깃 매크로는 명령줄에 남는다. `ROCKS_BUILD_*`, `ROCKS_PLATFORM_*`, `ROCKS_LOG_COMPILED_MINIMUM`, `NOMINMAX` 등 `CompileFlags`와 타깃 인자가 여기에 해당한다. 타깃의 모든 TU에 같으므로 PCH 호환을 깨지 않는다.
- 정의 헤더는 선언 입력이다(내용 해시). define 문자열은 지금처럼 InputValidation 검사를 받는다.
- 리플렉션 AST 생성(MetadataRunner)도 `--clang-arg=/D` 대신 같은 정의 헤더를 강제 include한다. 그래서 정의 헤더는 메타 생성 전에 쓴다. AST와 native compile의 정의가 갈라지지 않게 하는 것이 목적이다(`NativeCompileProfile`의 공유 원칙).

### 8.3 강제 include 순서와 PCH 앞부분

모든 PCH 사용 TU의 강제 include 순서는 다음과 같다.

| 순서 | 모듈 PCH | 공유 PCH | PCH 없음 |
|---|---|---|---|
| 1. PCH 앞부분 | 모듈 정의 헤더(전체) | 공유 정의 헤더 | — |
| 2. PCH 헤더 | 모듈 PCH 헤더 | 공유 PCH 헤더 | — |
| 3. 추가 정의 | 없음 | 소비 모듈 추가 정의 헤더 | 모듈 정의 헤더(전체) |

- PCH 생성 명령은 1과 2를 같은 순서로 강제 include한다. 소비 TU의 1은 생성 때와 **같은 파일**(같은 경로와 같은 내용)이다.
- clang-cl `/Yu`는 through header 앞의 `/FI`를 검증 없이 건너뛴다. 실측: 생성은 `/FIdefs0.h`, 소비는 `/FIdefs.h /FIpch.h /Yupch.h`로 했더니 `/WX`에서도 성공했고 소비 소스에서 `defs.h`의 정의가 보이지 않았다. 그래서 PCH 앞부분의 내용 해시는 PCH 변형 key에 들어간다(8.4절). 소비 TU가 다른 앞부분을 요구하면 다른 변형이 된다.
- 모듈 PCH는 모듈 정의 전체가 PCH 안에 있으므로 추가 정의 문제가 없다.

### 8.4 공유 PCH와 PCH 변형

**제공**
- 모듈 규칙의 `SharedPchHeader`(그 모듈 public include 기준 철자)가 공유 PCH를 제공한다.
- 엔진의 제공 모듈은 Core(`Core/CoreSharedPch.h`)와 ObjectRuntime(`Object/ObjectSharedPch.h`)이다. 고른 방법과 헤더 목록은 이 절 끝의 "엔진의 제공 모듈과 헤더 목록"에 있다. Engine은 아직 제공하지 않는다(같은 곳).

**선택**
- 소비 모듈은 도달 집합에 있는 제공 모듈 중 **다른 제공 모듈을 가장 많이 포함하는 것**을 고른다. Public·Private 의존이 생기면([빌드 타깃](BuildTargets.ko.md) 3.5절) 후보는 "도달 집합"이 아니라 "컴파일 환경에 보이는 제공 모듈"이고, "public include closure"는 그 문서의 공개 환경이다(UE `InternalGetAllDependencyModulesForPCH`와 같다). 동률이면 이름 ordinal 순이다.
- 제공 모듈이 소비 모듈에 의존하면(소비 모듈이 제공 모듈의 의존 closure에 있으면) 그 PCH를 쓸 수 없다. UE와 같다(`Configuration/UEBuildTarget.cs:4854–4895`).

**공유 정의**
- 공유 정의 헤더는 제공 모듈의 public define과 그 전파분만 담는다.
- 소비 모듈의 추가 정의 헤더는 공유 정의에 **없는** 정의만 담는다.
- 같은 이름에 다른 값이 있으면 그 소비 모듈은 공유 PCH를 쓰지 않는다. 진단 로그 한 줄을 남기고 PCH 없이 진행한다.
- 모듈 API 매크로가 없으므로 UE식 `#undef` 재정의는 두지 않는다.

**PCH 변형 key**
- key = (경로가 아닌 compile 인자 전체, PCH 앞부분 내용, PCH 헤더 철자)의 해시다. 경로가 아닌 인자에는 툴체인 인자, 구성 플래그, 최적화 재정의, sanitizer 계측, ThinLTO, 경고, 모듈 `AdditionalCompileArguments`, 타깃 인자가 모두 들어간다.
- 경로 인자(include 디렉터리, 소스, 출력, 강제 include 경로, PCH 사용 인자)는 key에서 뺀다. include 목록은 소비 모듈마다 달라서, 넣으면 공유 PCH가 사실상 모듈마다 하나가 된다.
- 같은 공유 PCH 헤더라도 변형 key가 다르면 별도 PCH action이다. 위치는 `<out>/Pch/<제공 모듈>/<변형 key 앞 12자리>/`다. 변형 수는 빌드 보고에 남긴다.
- UE처럼 축을 코드로 나열하지 않는다. 축을 하나 빠뜨리면 잘못된 PCH를 쓰게 되고, clang은 그중 일부만 거부하기 때문이다. 대가로, PCH를 나눌 필요가 없는 인자 차이도 변형이 된다.
- PCH를 켠 빌드가 성공하면 이번 plan이 쓰지 않은 변형 디렉터리를 지운다. 구성은 출력 디렉터리(`out/<preset>`)마다 하나이므로, 남는 변형은 인자가 바뀌어 낡은 것이다. PCH를 끈 빌드는 지우지 않는다.

**공유 PCH 사용 검사**(빌드 오류)

빌드는 공유 PCH를 쓰기 전에 다음을 확인한다. PCH 빌드의 의존 기록이 PCH 헤더 closure이고, include 디렉터리 이름 목록이 이미 있으므로 검사 비용은 작다.
- **include 포함**: 제공 모듈의 public include closure가 소비 모듈 include 목록에 모두 들어 있어야 한다. 의존 전파로 원래 성립하지만 검사로 보장한다.
- **shadow 없음**: PCH closure의 헤더가 include 디렉터리 D에서 철자 S로 찾아졌다면, 소비 모듈 include 목록에서 D보다 앞선 디렉터리에 S가 없어야 한다. 있으면 PCH 없이 컴파일할 때 다른 헤더가 잡힌다.
- **추가 정의**: 소비 모듈 추가 정의의 이름이 PCH closure 헤더의 전처리 조건(`#if`·`#ifdef`·`#ifndef`·`#elif`·`defined()`)에 나타나면 안 된다.
  - closure 안 `#define`의 본문에 그 이름이 있으면 그 매크로 이름도 대상에 더하고, 더 늘지 않을 때까지 반복한다. `#define X FOO` 뒤 `#if X` 같은 간접 참조를 잡기 위해서다.
  - 토큰 단위로 훑고 주석과 문자열은 제외한다.
  - PCH 안 헤더는 추가 정의를 보지 못한 상태로 컴파일되어 있으므로, 검사가 없으면 같은 헤더가 PCH 사용 여부에 따라 다르게 컴파일된다. clang++는 추가 `-D`를 경고 없이 받고, clang-cl은 `/D`에만 `-Wclang-cl-pch`를 낸다. 정의 헤더로 옮기면 두 툴체인 모두 아무것도 알리지 않는다(실측: PCH 0, 비PCH 1).

오류 메시지는 정의 이름, 헤더, 줄을 알린다. 해결 방법은 그 정의를 제공 모듈 쪽 public define으로 올리거나 PCH 헤더에서 해당 헤더를 빼는 것이다.

**엔진의 제공 모듈과 헤더 목록**(2026-11-20)

방법
- 번역 단위(`.cpp`)마다 public 헤더의 include closure를 정적으로 구하고(꺾쇠는 public include 철자, 따옴표는 상대 경로, 전처리 조건은 무시), 헤더마다 닿는 번역 단위 수를 셌다. 안정성은 최근 4주 커밋 수(`git log --since="4 weeks ago"`)로 보았다.
- 의존 기록(`.deps`, 4.1절)이 더 정확한 자료지만, 고를 때 이 작업 트리에는 UB0 뒤의 빌드 출력이 없었다(끝에 한 번만 빌드하기로 한 결정). 정적 분석은 전처리 조건을 무시하므로 닿는 수를 조금 크게 센다. 최종 검증 빌드의 `--timing` 집계로 사후 확인한다(개정 이력).
- 기준: 제공 모듈 의존 closure의 public 헤더 가운데 다음을 모두 만족하는 것만 넣는다.
  - 그 PCH를 쓰게 될 번역 단위의 30% 이상이 닿는다. 대상은 그 제공 모듈이 선택되는 런타임 모듈의 소스이고, ObjectRuntime은 시험 소스(시험 실행 파일이 이 PCH를 쓴다)를 더한다.
  - 자신과 include closure의 어느 헤더도 최근 4주 동안 12회 넘게 바뀌지 않았다. PCH에 든 헤더가 바뀌면 그 PCH를 쓰는 번역 단위가 모두 다시 컴파일되기 때문이다.
  - 소비 모듈마다 다른 정의(`ROCKS_MODULE_NAME`·`ROCKS_MODULE_TYPES`·모듈 private define)를 전처리 조건에 쓰지 않는다(추가 정의 검사).
  - Private 헤더와 생성 헤더는 넣지 않는다.

Core — `Core/CoreSharedPch.h`(소비 번역 단위 257개 기준, 괄호는 닿는 비율 %)
- Base: `Types.h`(98.8), `TypeTraits.h`(98.8), `NumericLimits.h`(98.8), `Checked.h`(59.9), `Compiler.h`(47.1), `Guid.h`(45.5), `Endian.h`(41.2)
- PlatformBase: `Platform/Fatal.h`(95.7), `Platform/Memory.h`(92.2), `Platform/Sync.h`(42.4), `Platform/Atomics.h`(40.1)
- Core: `Error.h`(96.1), `Move.h`(95.7), `Traits.h`(95.7), `Result.h`(95.3), `Memory/Allocator.h`(86.4), `Containers/Span.h`(82.9), `Text/StringView.h`(76.3), `Containers/ContainerDetail.h`(53.3), `Algorithms.h`(47.9), `Containers/Array.h`(47.9), `Text/String.h`(42.8), `Encoding/Sha256.h`(39.3), `Memory/UniquePtr.h`(31.9), `Hash.h`(30.4), `Text/Name.h`(30.4)

ObjectRuntime — `Object/ObjectSharedPch.h`(소비 번역 단위 567개 기준: 런타임 124 + 시험 443). Core 목록 전체에 다음을 더한다.
- Core: `Assert.h`(62.4), `Memory/SystemAllocator.h`(60.8), `Containers/HashMap.h`(52.0), `Text/FormatParse.h`(51.3), `Text/StringBuilder.h`(51.3), `Lifetime/ModuleLease.h`(49.4), `Optional.h`(49.0), `Containers/OrderedMap.h`(46.4), `Guid.h`(45.9), `Memory/SharedPtr.h`(34.9)
- Localization: `Catalog.h`(49.4), `Text.h`(49.4)
- ObjectRuntime: `ObjectHandle.h`(48.7), `AssetId.h`(45.7), `ReferenceObserver.h`(45.0)

넣지 않은 것
- `Object/Object.h`와 그 include closure. 닿는 비율은 48%(런타임 소비 모듈만 보면 81%)지만 closure의 `Meta/TypeSchema.h`(4주 28회), `Object/RuntimeTypeOps.h`(32회), `Object/ObjectRegistry.h`(47회)가 자주 바뀐다. 넣으면 이 헤더들이 바뀔 때마다 시험 실행 파일의 모든 묶음이 다시 컴파일된다(넣지 않으면 그 헤더를 쓰는 디렉터리의 묶음만이다). 객체 계층 헤더가 안정되면 다시 정한다.
- `Core/Module/ModuleEntry.h`. `ROCKS_MODULE_TYPES`를 전처리 조건에 쓴다.
- Engine 제공 모듈. World·Engine의 public 헤더는 닿는 번역 단위가 2% 안팎이고 `World/World.h`는 4주 17회 바뀌었다. 지금 Engine에 닿는 소비자는 시험 실행 파일뿐이어서, 넣으면 시험 전체가 World 헤더에 묶인다. 게임 쪽 코드가 생기고 World 헤더가 안정되면 정한다(UE `EngineSharedPCH` 대응).
- Graphics 제공 모듈. Graphics 계열 소비 번역 단위(86개)가 함께 쓰는 것은 Core 목록과 작은 GraphicsTypes 열거 헤더이고, `Graphics/Device.h`(20회)·`Graphics/DeviceTypes.h`(24회)는 자주 바뀐다. 이 모듈들은 Core의 PCH를 쓴다.

결과와 기대 효과
- 계획(EngineTests·Foundation·Sandbox Debug)에서 변형은 제공 모듈마다 하나다(PCH action 2개). EngineTests 논리 명령 1,565개 중 634개가 ObjectRuntime, 213개가 Core의 공유 PCH를 쓴다. 나머지는 C 소스·외부 라이브러리·생성 TU·공개 헤더 단독 컴파일 검사·Core 아래 모듈(Base·PlatformBase)이다.
- 엔진 헤더는 STL을 거의 쓰지 않아 파싱이 싸다. clang-cl `/Zs` 단일 TU 실측으로 `Core/CoreSharedPch.h` 전체가 0.15 s, `Object/ObjectSharedPch.h` 전체가 0.19 s다(프로세스 기동 0.05 s 포함). 공유 PCH의 절감은 번역 단위당 0.1–0.15 s 수준이다.
- 검증 빌드의 `--timing` 집계(Foundation Debug)로 보면 PCH를 쓰지 않는 번역 단위에서 `Core/Result.h`의 포함 시간이 평균 0.13 s(348개)이고, 그 거의 전부가 `Core/Move.h`가 include하는 `<new>`다(0.126 s — MSVC 헤더에서 `<exception>`·`<type_traits>`를 끌어온다). 엔진 소유 코드(외부 라이브러리와 공개 헤더 단독 컴파일 검사 제외)는 118 번역 단위·219.7 s이므로, 공유 PCH가 줄이는 몫은 소유 코드 컴파일 시간의 7% 안팎이다(추정 — PCH만 끈 대조 빌드는 돌리지 않았다). 작업 파일 분리(7절)로 단독 컴파일되는 파일의 반복 시간에도 같은 만큼 듣는다.
- 같은 집계로, 남은 컴파일 시간의 큰 몫은 공개 헤더 단독 컴파일 검사(405 번역 단위, 113.7 s — 그중 `<new>` 사슬이 45%)와 외부 라이브러리(190 번역 단위, 125.0 s)다. `Core/Move.h`가 `<new>` 대신 placement new 선언만 갖게 하면 PCH를 쓰지 않는 번역 단위마다 0.13 s가 준다(아래 "placement new 헤더"로 반영했다).

**placement new 헤더(`Base/PlacementNew.h`)**
- 문제: `::new (storage) T(...)`를 쓰려면 `operator new(size_t, void*)` 선언이 보여야 하고, 표준은 그것을 `<new>`에 둔다. MSVC STL의 `<new>`는 `<exception>`을 include하고 그것이 `<type_traits>`·`<cstdlib>` 등을 끌어와, 번역 단위마다 0.126 s가 든다. `Core/Move.h`가 `<new>`를 include하므로 Core 헤더를 쓰는 거의 모든 번역 단위가 이 값을 낸다. 공유 PCH를 쓰는 번역 단위는 PCH가 한 번 내지만, 공개 헤더 단독 컴파일 검사(PCH·유니티 없음 — 10절)와 PCH를 쓰지 않는 번역 단위는 매번 낸다.
- 구조: `Base/PlacementNew.h`가 placement new 선언만 여는 가장 가벼운 헤더를 고른다.
  - `<vcruntime_new.h>`가 있으면(MSVC 런타임) 그것을 include한다. MSVC의 `<new>`가 `<exception>` 다음에 include하는 바로 그 헤더이고, `operator new`·`operator delete` 선언, placement new·delete, `std::align_val_t`·`std::nothrow`를 담는다. `<vcruntime.h>`만 끌어온다.
  - 없으면(libc++·libstdc++) `<new>`를 include한다. 그쪽 `<new>`는 가볍다.
  - 판정은 `__has_include`로 한다. OS가 아니라 표준 라이브러리 구현을 가르는 것이어서 플랫폼 분기(`ROCKS_PLATFORM_*`)를 쓰지 않는다.
- 규칙: 엔진 소스와 헤더는 placement new를 쓸 때 `<new>` 대신 `<Base/PlacementNew.h>`를 include한다(`Core/Move.h`의 `ConstructAt`·`InitializeAt`을 거치면 따로 include하지 않아도 된다). 시험 소스는 `<new>`를 include할 수 있다(전역 `operator new` 교체처럼 `<new>`의 다른 것이 필요한 시험이 있다). 정책 검사 `placement-new-header`(9.2절)가 시험 밖의 `#include <new>`를 거부한다.
- 한계: `<vcruntime_new.h>`는 `std::launder`, `std::bad_alloc`, `std::hardware_destructive_interference_size`를 선언하지 않는다. 엔진 코드는 지금 이것들을 쓰지 않는다. 필요해지면 그 소스에서 정책 예외와 함께 `<new>`를 include한다. `<new>`가 끌어오던 `<cstdlib>`·`<cstdint>` 등에 기대던 소스는 필요한 헤더를 직접 include해야 한다.
- UE 대응: UE 5.8은 `<new>`를 그대로 include한다(`HAL/PlatformCrt.h`, `Templates/MemoryOps.h`, `Templates/TypeCompatibleBytes.h`, `Misc/Launder.h`). 거의 모든 번역 단위가 공유 PCH를 쓰므로 비용이 PCH에 한 번 들어간다. Rocks는 공개 헤더 단독 컴파일 검사가 PCH 없이 헤더마다 번역 단위를 하나씩 만들기 때문에 이 비용이 그대로 곱해진다. 그래서 UE와 달리 가벼운 헤더로 바꾼다.
- 버린 대안:
  - `void* operator new(usize, void*) noexcept;`를 직접 선언하는 방식. libc++는 이 함수를 `_LIBCPP_HIDE_FROM_ABI`(`abi_tag` 속성)로 선언하므로, 속성 없는 선언이 앞서 있으면 `<new>`를 함께 include한 번역 단위가 오류다(NDK r29 libc++ `new:245`로 확인).
  - `__PLACEMENT_NEW_INLINE`을 정의하고 inline 정의를 직접 두는 방식. 런타임 헤더의 내부 guard 이름에 기댄다. 같은 선언을 담은 헤더를 include하는 쪽이 낫다.
  - 꼬리표 인자를 받는 자체 placement 연산자(`new (Placement{}, storage) T`). 표준이 예약한 형식이 아니어서 컴파일러가 호출과 null 검사를 만든다(Debug에서 컨테이너 원소 생성마다 함수 호출). 모든 호출부도 바꿔야 한다.
- 수치(Foundation Debug 검증 빌드의 `--timing` 집계, 같은 기기): 아래 표. 전은 UB6 최종 검증 빌드, 후는 이 변경의 검증 빌드다.

| 항목 | 전 | 후 |
|---|---|---|
| 공개 헤더 단독 컴파일 검사 | 405 번역 단위, 113.7 s(평균 0.281 s) | 417 번역 단위, 88.7 s(평균 0.213 s) |
| `Core/Result.h` 포함 시간(번역 단위 평균) | 0.13 s(348개) | 0.016 s(361개) |
| placement new 선언을 여는 헤더(번역 단위 평균) | `<new>` 0.126 s | `<vcruntime_new.h>` 0.009 s(370개) |

- 두 빌드는 커밋이 다르고(공개 헤더가 12개 늘었다) 기기 부하도 달라, 검사 합계의 차이(25.0 s)가 전부 이 변경의 몫은 아니다. 이 변경의 몫은 헤더 행에 보인다: `<new>`·`<exception>`·`<type_traits>`는 집계의 상위 헤더 100개에서 빠졌고, 번역 단위마다 0.12 s쯤 줄었다.
- 유니티·PCH를 끈 EngineTests 빌드(번역 단위 1,837개를 단독으로 컴파일)가 그대로 통과했다. `<new>`가 끌어오던 헤더에 기대던 소스는 없었다.


**설치 SDK 게임 빌드**
- SDK manifest가 제공 모듈의 `SharedPchHeader` 철자와 공유 정의를 함께 나른다.
- 게임 빌드는 SDK 헤더로 공유 PCH를 자기 출력에 직접 만든다(게임 인자의 변형).
- prebuilt 엔진 라이브러리는 PCH와 무관하다(`Rocks.Build.Cli/InstalledSdk.cs:324–331`).

### 8.5 툴체인별 생성·사용

| 툴체인 | 생성 | 사용 |
|---|---|---|
| clang-cl(Windows) | 빈 stub `.cpp`에 `/FI<앞부분> /FI<헤더> /Yc<헤더> /Fp<pch> /Fo<stub obj>` | `/FI<앞부분> /FI<헤더> /Yu<헤더> /Fp<pch>`, 그다음 `/FI<추가 정의>` |
| clang++(macOS·Android NDK) | `-include <앞부분> -x c++-header <헤더> -o <pch>` | `-include-pch <pch>`, 그다음 `-include <추가 정의>` |

- 공통 인자는 `-Xclang -fno-pch-timestamp`(PCH 바이트 결정성)이고 clang-cl에서도 같은 철자다. 실측: `-fno-pch-timestamp`를 그대로 쓰면 cc1 전용이라 clang++는 오류를 내고, clang-cl `/clang:` 전달은 "unknown argument ignored"로 무시된다(`/WX`에서는 오류). UE도 `-Xclang` 철자를 쓴다(`Platform/Clang/ClangToolChain.cs:517`).
- `-fpch-instantiate-templates`를 켠다(clang-cl `/Yc`는 기본값). UE clang 경로와 같다(`ClangToolChain.cs:618`).
- **낡은 PCH 방지는 Rocks key가 맡는다.** timestamp를 끈 PCH에서 clang의 입력 검증은 크기만 본다. 실측: `-fpch-validate-input-files-content`를 붙여도 같은 크기의 내용 변경(12345→54321)을 거부하지 않고 낡은 PCH로 조용히 컴파일했다. 그래서 이 플래그는 쓰지 않는다. PCH action은 4.1의 2단 key로 다시 만들고, 소비 TU는 PCH 기록 해시를 선언 입력으로 갖는다(4.3절).
- clang-cl `/Yc`는 stub object(실측 약 3.5 KB)도 만든다. 원자적 게시를 위해 출력으로 선언하지만 링크하지 않는다. DLL이 없고 `-fpch-codegen`도 쓰지 않으므로 필요가 없다.
- PCH action의 출력은 PCH 파일, stub object(clang-cl), 의존 기록이다. 소비 compile action은 PCH action에 의존한다.
- `-fpch-codegen`과 `-fpch-debuginfo`는 PCH 안 inline 함수의 코드 생성을 한 번만 하게 하며 큰 효과가 보고되어 있다. 하지만 PCH object를 링크해야 하고 정적 라이브러리 경계와 얽힌다. UB6 측정 뒤 별도로 결정한다(UD20).
- Release ThinLTO와 Asan에서도 PCH를 쓴다. 변형 key가 구성별로 나뉜다. 디버그 정보(`/Z7`·`-g`)는 PCH와 함께 쓸 수 있다.

### 8.6 compile DB

`compile_commands.json`은 원본 소스마다 한 항목이고, 명령은 **논리 명령**이다.
- 유니티를 쓰지 않는다.
- PCH 바이너리(`/Yu`·`-include-pch`) 대신 PCH 앞부분, PCH 헤더, 추가 정의 헤더를 같은 순서로 강제 include한다.
- 묶음 TU용 `/I<root>`는 넣지 않는다.

clangd와 정책 감사는 실제 컴파일과 같은 매크로·include를 보면서 원본 파일 단위로 동작한다. clangd는 다른 버전의 PCH 바이너리를 읽지 못할 수 있다. 묶음 파일과 PCH stub은 compile DB에 넣지 않는다.

README의 "action key·`compile_commands.json`·BuildInfo manifest는 모두 `NativeAction.Arguments`로 만든다"는 계약은 다음으로 바뀐다(17절). action key는 물리 action 인자로, compile DB와 BuildInfo manifest는 논리 명령으로 만든다.

### 8.7 도구 빌드 ID

- BuildInfo manifest의 compile 부분은 물리 action 대신 원본 소스마다의 논리 명령(8.6절)과 그 소스 내용으로 만든다. 그 결과 유니티·PCH 모드와 작업 파일 집합이 ID를 바꾸지 않는다. 커밋만 해서 작업 파일이 바뀌어도 ID가 같다.
- PCH 앞부분, PCH 헤더, 정의 헤더는 생성 입력으로서 내용 해시를 넣는다.
- 경로 정규화(`BuildInfo.cs` `NormalizeArgument`)에 새 경로 인자를 더한다: `/FI`·`-include`, `/Fp`·`-include-pch`, `/Yc`·`/Yu`, `/clang:-MF`·`-MF`. 정규화하지 못한 호스트 경로는 지금처럼 예외다.
- archive와 link 부분은 지금처럼 물리 action으로 만든다. 묶음 object는 묶음 구성이 바뀌면 이름이 바뀌므로, link의 object 목록은 원본 소스 키 목록(`obj:<소스 키>`)으로 대신 적는다.

## 9. 묶음 충돌

### 9.1 위험

묶음 TU에서는 구성원 파일의 파일 지역 선언이 한 TU에 모인다.
- **컴파일 오류로 드러나는 것**
  - 익명 namespace, `static`, namespace 범위 `constexpr` 변수의 같은 이름 재정의
  - 같은 이름의 지역 타입
- **조용히 바뀌는 것**
  - 파일 지역 함수 overload: A 파일의 `Helper(int)`와 B 파일의 `Helper(long)`이 한 TU에 있으면 B의 `Helper(5)`가 A의 함수를 고른다.
  - 파일 범위 `using namespace`가 뒤 파일의 이름 탐색을 바꾼다.
  - `.cpp`에서 정의하고 `#undef`하지 않은 매크로가 뒤 파일에 남는다.
  - `#pragma` push/pop 불균형과 `#pragma pack`
  - 첫 include 앞의 `#define`. 6.1절에서 자동으로 단독 TU로 뺀다.

검증 빌드(유니티 끔)는 조용히 바뀌는 쪽을 잡지 못한다. 단독으로는 맞게 컴파일되기 때문이다.

### 9.2 정책 검사(`Rocks.Tooling.Checks`)

묶음 구성은 작업 파일 집합과 상한에 따라 바뀐다. 그래서 검사는 **모듈 단위**다. 어느 두 파일이 같은 묶음에 들어가도 안전해야 한다. 대상은 `Unity`를 켠 모듈만이다. UB1에서 검사를 넣어도 정리 전 모듈(`Off`)은 실패하지 않는다.

| 검사 | 규칙 |
|---|---|
| 파일 지역 이름 | 같은 모듈의 두 `.cpp`가 같은 이름을 최상위 익명 namespace나 namespace 범위 `static`·`constexpr`로 선언하면 오류. overload 차이(인자 타입)도 같은 이름으로 본다 |
| 파일 범위 using | 파일 범위 `using namespace`는 `rocks`와 그 하위 namespace만 허용한다. 2026-11-18 기준 322개 파일이 `using namespace rocks`를 쓴다. 그 밖은 함수나 namespace 안으로 옮긴다 |
| `.cpp` 매크로 | `.cpp`에서 `#define`한 매크로는 그 파일 끝까지 `#undef`해야 한다. 첫 include 앞의 `#define`은 6.1절 자동 제외 대상이므로 이 검사에서 뺀다 |
| pragma 균형 | `#pragma warning(push)`, `clang diagnostic push`, `pack(push)`는 같은 파일에서 pop해야 한다 |
| placement new 헤더(`placement-new-header`) | 시험 소스 밖의 `#include <new>`는 오류. `<Base/PlacementNew.h>`를 쓴다(8.4절 "placement new 헤더") |
| 매크로를 남기는 헤더(`windows-macro-leak`) | Windows 플랫폼의 모든 모듈(외부 라이브러리 원본 제외)에서, C++ 소스·PCH 헤더와 거기서 닿는 저장소 헤더가 `WindowsHeadersBegin.h`/`WindowsHeadersEnd.h` 쌍 밖에서 include할 수 있는 것은 저장소 헤더, 생성 헤더, 깨끗한 시스템 헤더 목록의 헤더뿐이다. 그 밖의 include(Windows SDK 헤더, SDK 헤더를 여는 외부 라이브러리 헤더, 리터럴이 아닌 include)가 쌍 밖에 있으면 오류. 쌍이 맞지 않아도(Begin 없는 End, 닫지 않은 Begin), 쌍 안에 저장소 헤더를 두어도 오류. 유니티를 끈 모듈과 `UnityExclude`의 소스도 본다(6.1절) |

- 검사는 토큰 단위 근사다. 주석, 문자열, 전처리 조건을 처리하지만 컴파일러 AST 수준의 정확성은 요구하지 않는다. 놓친 것은 드러나는 경우 컴파일 오류로 남는다.
- 이름 충돌은 파일마다 고유한 이름의 namespace로 옮기거나(예: `namespace rocks::detail::JsonReaderLocal`) 이름을 바꿔서 푼다. 익명 namespace 자체는 금지하지 않는다.

### 9.3 정리 범위

1. 런타임 모듈(356개)을 먼저 정리하고 유니티를 켠다(UB2).
2. 시험 실행 파일 모듈(FoundationTests·EngineTests, 373개)을 정리하고 켠다(UB3). 그 전까지 시험 모듈은 `Unity = Off`다.

정리 커밋에는 동작이 바뀌지 않는 이동과 이름 변경만 담는다.

## 10. include 위생

- `.cpp`의 첫 include는 짝 헤더(있으면)다. 작업 파일 분리(7.2절)가 이 규칙을 쓰고, 정책 검사가 확인한다(UE `bEnforceIWYU`, `UEBuildModuleCPP.cs:2427–2475`).
- 소스는 PCH 헤더와 정의 헤더를 직접 include하지 않는다(8.1절). 정책 검사 `pch-header-include`가 확인한다(`Rocks.Tooling.Checks/PolicyChecks.Pch.cs` — 규칙의 `PrivatePchHeader`·`SharedPchHeader`가 가리키는 헤더를 include하는 소스·헤더를 거부한다. PCH 헤더가 다른 PCH 헤더를 include하는 것만 허용한다).
- 공개 헤더 단독 컴파일 검사(`PublicHeaderChecks` 모듈)는 PCH와 유니티 없이 돈다(`Unity = Off`, `PchUsage = Off`). 공유 PCH를 쓰면 PCH에 든 헤더가 빠진 include를 메워 검사가 무력해진다.
- UE `-Mode=IWYU` 같은 자동 include 수정 도구는 이 스펙 범위 밖이다.

## 11. 검증 빌드와 CI

| 변형 | 유니티 | PCH | 작업 파일 분리 | 용도 |
|---|---|---|---|---|
| 기본 | 켬 | 켬 | 로컬은 켬, CI는 끔 | 일상 빌드 |
| 검증 | 끔 | 끔 | 끔 | include 누락과 PCH 의존 검출 |

- CLI는 `--unity on|off`, `--pch on|off`, `--adaptive on|off`이다. 기본은 모두 `on`이고 CI는 `--adaptive off`다. 값은 object 변형 이름(6.4절)과 action 인자에 반영된다. 도구 빌드 ID와 compile DB에는 반영되지 않는다(8.6·8.7절).
- CI는 기본 변형으로 전체 시험을 돌리고, 검증 변형으로 Debug 빌드 1개(Windows)를 돈다. 검증 변형은 링크까지 하고 시험은 돌리지 않는다. 컴파일 단위의 문제를 찾는 것이 목적이다.

## 12. 측정

**`--timing`**
- compile action과 PCH action에 clang `-ftime-trace`를 붙이고, 빌드가 끝나면 결과 JSON을 집계한다.
- 출력은 `<out>/Timing/<Target>.timing.json`이다. TU별 시간, 헤더별 누적 파싱 시간 상위 목록, 템플릿 인스턴스화 상위 목록을 담는다.
- UE `-Timing`·`AggregateClangTimingInfo`(`VCToolChain.cs:2644–2700`)에 대응한다.
- 공유 PCH 헤더 구성(8.4절)은 이 목록으로 정한다.

**묶음 크기 실측**
- `Rocks.Build.Cli unity-profile --module <M>`은 다음 조합으로 clean compile을 실측한다.
  - 유니티 끔
  - `UnityAnchorFiles` 4단계(8·16·24·32)
  - 상한 3단계(256·384·512 KiB)
- 결과는 모듈별 CPU 시간, 벽시계 시간, 최대 메모리 표로 낸다.
- 모듈 재정의 값은 이 결과로만 정한다. UE `-Mode=ProfileUnitySizes`에 대응한다.
- **아직 구현하지 않았다**(UB6, 2026-11-20). 조합 13개를 모듈마다 clean compile하면서 프로세스별 CPU 시간과 최대 메모리를 재려면 실행기의 프로세스 계측과 모듈 단독 실행이 필요한데, 측정용 빌드를 돌리지 않기로 한 이번 작업에서는 결과를 쓸 곳이 없다. 그때까지 `UnityAnchorFiles` 16과 상한 384 KiB를 그대로 쓰고, 모듈 재정의는 두지 않는다.

**기준 시나리오**(계획 27.8)
- 같은 장비, 같은 commit에서 기존 방식(UB0 전), UB0, UB2(유니티), UB5(PCH)를 각각 측정한다.
- 시나리오는 다음 넷이다.
  - clean build
  - I0 무변경
  - I3 private `.cpp` 하나(작업 파일 분리 켬)
  - I4 Core 공개 헤더 하나
- 각각 20회 이상 반복하고 p50, p95, 최대, 최대 메모리를 기록한다.

**합격**
- 조건:
  - stale hit 0
  - 검증 변형 통과
  - I0·I3 회귀 없음(측정 오차를 넘어 느려지지 않음)
  - clean·I4 개선
- 개선이 없으면 기본값(구간 파일 수, 상한, PCH 구성)을 조정하고, 그래도 없으면 그 기능의 기본을 끈다.

## 13. 병렬도

묶음 TU는 TU 하나가 쓰는 메모리가 크다. 현재 실행기는 `MaxParallelism`만 본다.

UE 기준은 다음과 같다. UBA 실행기 기본은 action당 1.5 GiB(`Executors/UnrealBuildAccelerator/UnrealBuildAcceleratorConfig.cs:394`)이고, Target 기본 `MemoryPerActionGB`는 0(`TargetRules.cs:2927`)이다. 계산식은 `System/Utils.cs:1030–1100`에 있으며, 물리 코어 수와 여유 메모리 ÷ action당 메모리 중 작은 값을 쓴다.

UB6에서 묶음 TU와 PCH action의 최대 메모리를 실측해, 동시 실행 수를 메모리로도 제한할지 정한다(UD20). 제한은 실행 슬롯 수를 결정적으로 계산해야 하며, 실행 중 메모리 관측에 따라 바뀌면 안 된다.

**결정**(UB6, 2026-11-20): 메모리 기반 제한은 두지 않는다. 실행기는 지금처럼 `MaxParallelism`(기본 `min(8, 논리 코어 수)`)만 본다. 최대 메모리는 따로 실측하지 않았고, 끝의 검증 빌드(EngineTests·Foundation Debug, 8 병렬)에서 메모리 부족이 나오는지로 판단한다(결과는 개정 이력). 부족이 나오면 위 방식(결정적 슬롯 수)으로 더한다.

## 14. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| UD1 | 정밀 헤더 의존(2단 key: 선언 입력 key + 의존 기록 검증, include 디렉터리 이름 목록, 툴체인 소유 헤더 제외)을 유니티·PCH 앞 단계(UB0)로 둔다 | 확정 |
| UD2 | compile identity에서 모듈 전체 소스 목록을 빼고 TU 구성만 넣는다 | 확정 |
| UD3 | 큰 산출물 검증은 (크기, 수정 시각, 파일 ID)가 모두 같으면 기록 해시를 쓰고, 다르면 다시 해시한다 | 확정 |
| UD4 | 묶음 경계는 전체 목록 기준 경로 해시 anchor(고정 간격 16)로 구간을 나누고, 구간이 상한(384 KiB, 실제 바이트)을 넘으면 파일 수로 균등 분할한다. 구성원 하나면 단독 TU | 확정 |
| UD5 | BuildInfo·규칙 생성 TU, C 소스, 외부 라이브러리 모듈은 묶지 않는다. 리플렉션 생성 소스는 자기들끼리 묶는다(손으로 쓴 소스와 섞지 않는다. 모듈의 `Unity` 설정과 무관하게 기본으로 켜고 `ReflectionUnity = false`로 끈다 — UB1 결정, [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md) GSD9) | 확정 |
| UD6 | 묶음 파일은 루트 기준 `#include`와 묶음 TU 전용 맨 뒤 `/I<root>`를 쓴다. 이름은 첫 구성원 경로 해시. 이번 변형 디렉터리의 낡은 묶음과 object는 지운다 | 확정 |
| UD7 | 작업 파일은 git으로만 판별한다(XY 두 열, `?? M A T U R C`). timeout 없이 완료를 기다리고 실패는 오류다. CI는 `--adaptive off` | 확정 |
| UD8 | PCH 기본 모드는 모듈 PCH 우선, 없으면 공유 PCH. 소스는 PCH를 include하지 않는다. 모듈 PCH는 TU 3개 이상일 때만 만든다 | 확정 |
| UD9 | 모듈 정의는 생성 정의 헤더로 옮기고, 구성·플랫폼·타깃 매크로는 명령줄에 남긴다. AST 생성도 같은 헤더를 쓴다 | 확정 |
| UD10 | PCH 변형은 경로가 아닌 인자, PCH 앞부분, PCH 헤더 철자의 해시로 자동 구분한다. include 경로는 key에서 빼고 포함 관계와 shadow를 검사한다 | 확정 |
| UD11 | 소비 모듈의 추가 정의가 공유 PCH closure의 전처리 조건에 쓰이면(매크로 본문 간접 참조 포함) 빌드 오류 | 확정 |
| UD12 | 묶음 충돌은 `Unity`를 켠 모듈 단위 정책 검사로 막는다(파일 지역 이름, using, 매크로, pragma) | 확정 |
| UD13 | 충돌 정리는 런타임 모듈 → 시험 모듈 순서로 한다. UB1에서는 모든 모듈이 `Off`이고 정리한 모듈부터 켠다 | 확정 |
| UD14 | 검증 변형(유니티·PCH 끔)을 별도 object 변형 경로로 두고, CI가 Windows Debug 빌드 1개를 돈다 | 확정 |
| UD15 | compile DB는 원본 소스 단위의 논리 명령이고, PCH 앞부분·헤더·추가 정의를 강제 include한다 | 확정 |
| UD16 | 강제 include 순서는 [PCH 앞부분] → [PCH 헤더] → [추가 정의]이고, PCH 앞부분은 생성과 같은 파일이다 | 확정 |
| UD17 | 도구 빌드 ID의 compile 부분은 논리 명령으로 만들어 유니티·PCH·작업 파일 집합과 무관하게 한다. 새 경로 인자를 정규화한다 | 확정 |
| UD18 | 첫 include 앞에서 `#define`하는 파일은 자동으로 단독 TU로 빼고 PCH를 끈다 | 확정 |
| UD19 | PCH 바이트 결정성은 `-Xclang -fno-pch-timestamp`로 확보하고, 낡은 PCH 방지는 Rocks key가 맡는다(clang 입력 검증에 기대지 않는다) | 확정 |
| UD20 | `-fpch-codegen`·`-fpch-debuginfo`는 쓰지 않는다. 메모리 기반 병렬도 제한은 두지 않는다(13절). C++ modules는 도입하지 않는다 | 확정(2026-11-20, 권장안) |

## 15. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험(`Rocks.Build.Tests`)으로 검증한다.

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| UB0 | 2단 key와 의존 기록(4.1절), depfile 파서, 툴체인 소유 헤더 제외, compile identity 정리(4.2절), 큰 산출물 검증 memo(4.3절) | 공개 헤더 하나를 바꾸면 그 헤더를 실제로 include한 TU만 재컴파일한다. 새 shadow header와 `__has_include` 반례는 재빌드된다. no-op은 0 action이다. 의존 기록이 손상되면 재실행한다. depfile 파서는 공백·한글·괄호 경로 fixture를 통과한다 |
| UB1 | 유니티 묶기(6절), 모듈 규칙 `Unity`·`UnityBatchBytes`·`UnityAnchorFiles`·`UnityExclude`, include 앞 `#define` 자동 제외, CLI `--unity`, 변형 object 경로와 정리, compile DB·BuildInfo 논리 명령(8.6·8.7절), 묶음 충돌 정책 검사(9.2절). 모든 모듈 기본 `Off` | 묶음 구성 결정성 시험(같은 입력이면 같은 구성, 파일 추가·삭제·크기 변화가 그 구간만 바꿈), `__FILE__`이 단독 TU와 같음, 유니티 켬·끔의 도구 빌드 ID 동일, 검증 변형 빌드, 정책 검사 시험(규칙마다 위반·통과 fixture) |
| UB2 | 런타임 모듈 충돌 정리, 런타임 모듈 유니티 켬 | 기본·검증 변형 전체 통과, clean 시간 측정 기록 |
| UB3 | 시험 모듈 충돌 정리, 유니티 켬 | FoundationTests·EngineTests 기본·검증 변형 통과 |
| UB4 | 작업 파일 분리(7절), 정의 헤더(8.2절)와 MetadataRunner 전환, 모듈 PCH(8.1·8.3·8.5절) | 작업 파일을 편집하면 단독 TU와 그 파일이 빠진 묶음만 재컴파일한다. git 상태별 결정성(porcelain 상태 fixture)을 시험한다. 정의 헤더 변경이 key에 반영된다. 모듈 PCH 생성·사용·변형 key·stub object 게시를 시험한다. 같은 크기의 PCH 헤더 변경으로 PCH를 다시 만든다 |
| UB5 | 공유 PCH와 사용 검사(8.4절), PCH 변형 정리, SDK manifest 전달, `--timing` 집계(12절)로 공유 PCH 헤더 구성 | 제공 모듈 선택 규칙, 정의 충돌 시 PCH 없이 진행, 포함·shadow·추가 정의(간접 참조 포함) 위반 fixture, PCH 앞부분이 다르면 다른 변형, 설치 SDK 게임의 공유 PCH 빌드 |
| UB6 | `unity-profile`, 기준 시나리오 측정, 기본값 확정, UD20 결정 | 12절 합격 기준 |

## 16. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 헤더 의존 판정 | 타임스탬프 + 컴파일러 의존 목록 | 선언 입력 key + 의존 기록 내용 해시 검증 + include 디렉터리 이름 목록 | 브랜치 전환·touch로 인한 헛 재빌드 방지, negative include 검출 |
| 묶음 경계 | 크기 누적 | 경로 해시 anchor 구간 + 구간 안 균등 분할 | 파일 추가·편집이 뒤 묶음을 모두 밀지 않게 |
| 묶음 include | 절대 경로 | 루트 기준 경로 + 맨 뒤 `/I<root>` | 호스트 무관, `__FILE__`이 단독 TU와 같음 |
| 작업 파일 판별 | git 15초 timeout, Perforce read-only | git만, timeout 없음, 실패는 오류 | 결정성(시간에 기대지 않는다) |
| PCH 변형 축 | 코드로 나열한 축 | 경로가 아닌 인자와 PCH 앞부분의 해시 | 축 누락 방지 |
| 추가 정의 | 검사 없음 | PCH closure 전처리 조건 검사(간접 참조 포함), shadow 검사 | PCH 사용 여부에 따른 조용한 차이 방지 |
| 묶음 충돌 | 규약 | 모듈 단위 정책 검사, include 앞 `#define` 자동 제외 | overload·using·매크로의 조용한 변화 방지 |
| 생성 코드 묶기 | `.gen.cpp`를 앞에 두고 4 KB placeholder로 셈 | BuildInfo·규칙 생성 TU는 묶지 않는다. 리플렉션 생성 소스는 자기들끼리만 묶는다 | 생성 시점이 다른 TU는 묶으면 자주 다시 컴파일된다. 리플렉션 생성 소스는 파일 지역 이름·매크로 없이 나와 충돌이 없고, 손으로 쓴 소스와 섞지 않으면 모듈의 충돌 정리와 무관하게 묶을 수 있다 |
| 작은 게임 모듈 | 소스 32개 미만이면 유니티 끔 | 없음(구성원 하나만 단독) | 엔진·게임 구분 규칙이 필요 없다 |

## 17. 다른 문서 개정

- [엔진 구현 계획](EngineImplementationPlan.ko.md) 27.3절: 이 스펙 링크를 추가했다(이번 개정).
- `Programs/Rocks.Build/README.ko.md`: 단계마다 구현 범위와 증분 계약을 갱신한다(구현 시). 특히 다음 두 가지다.
  - "입력과 증분 계약"의 헤더 inventory 내용 해시 → 2단 key(UB0)
  - "긴 명령줄" 절의 "action key·`compile_commands.json`·BuildInfo manifest는 모두 `NativeAction.Arguments`" → action key는 물리 인자, compile DB와 manifest는 논리 명령(UB1)
- `Docs/Implementation/FoundationStatus.ko.md` C00 행: UB2·UB5가 끝나면 "PCH·unity" 미구현 표기를 고친다(구현 시).

## 18. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-18 | UE 5.8 UBT 조사(유니티·adaptive·PCH·IWYU·증분·측정)를 반영했다. 리뷰어 교차 검증으로 다음을 고쳤다: 묶음 경계 재설계, 루트 기준 include, `-Xclang -fno-pch-timestamp`와 clang 입력 검증의 한계, 강제 include 순서, 변형 key의 경로 제외와 shadow 검사, include 앞 `#define`, 도구 빌드 ID 논리 명령, depfile 2단 key, SDK 게임 공유 PCH, 낡은 산출물 정리, 인용 정정. 결정 UD1–UD20(UD20 보류), 단계 UB0–UB6. 설계 확정(사용자 승인 — 권장안) |
| 1(구현 기록 UB0) | 2026-11-20 | UB0 구현(`Programs/Rocks.Build`의 `Depfile`·`DependencyRecord`·`BuildInputs`·`FileIdentity`, `NativeAction.Key`, `IncrementalStore` 형식 2). 구현하며 정한 점. (1) 의존 기록은 action의 출력 파일 `<object>.deps`다(머리말 + `<해시> <경로>` 줄). object와 같은 게시 경로를 타므로 "출력과 함께 원자적으로 게시, 실패·취소는 기록 없음"이 따로 구현 없이 성립하고, 기록 파일 자체도 출력 검증(4.3절)을 받는다. (2) 기록의 해시는 빌드의 입력 메모(`BuildInputs` — 빌드 시작 때 읽은 내용, 파일마다 한 번)에서 가져온다. 빌드 중 변경은 기존의 빌드 전후 입력 비교(header 디렉터리 내용 포함)가 잡아 그 실행의 기록을 버리고, header 디렉터리 밖으로 벗어난 include(상대 경로 등)는 빌드 끝에 다시 읽어 대조한다. action마다의 "실행 중 입력 변경" 검사는 그 action의 입력 파일(소스)만 다시 읽는다. (3) compile identity에 전체 소스 목록을 넣은 원래 이유는 README의 "source 옆 local header 보수 추적"이었다 — 소스 디렉터리의 헤더 이름 목록과 의존 기록이 대신하므로 뺐다. (4) 파일 ID는 Windows에서 `GetFileInformationByHandle`의 볼륨 일련번호·파일 색인, 그 밖에서는 `stat`의 장치·inode다(.NET에 API가 없어 libc를 직접 부른다. 얻지 못하면 그 출력은 언제나 다시 해시한다 — 판정은 같고 느릴 뿐이다). (5) compile DB와 BuildInfo manifest의 compile 부분을 이 단계에서 이미 논리 명령(`NativePlan.LogicalCompiles`)으로 바꿨다 — depfile 인자(`-MD -MF`)가 물리 action에만 있어야 도구 빌드 ID와 clangd 입력이 바뀌지 않는다(8.6·8.7절의 나머지는 UB1). (6) 툴체인 소유 판정에는 `OwnedRoots`·`FingerprintPaths`·keyed `INCLUDE`를 쓴다. 확인: `Rocks.Build.Tests` 51묶음 통과(새 `DependencyTests` — 파서 fixture, 헤더 하나 변경 시 그 TU만, shadow header·`__has_include` 반례, 기록 손상·삭제, 소스 추가·삭제, 출력 검증 memo) |
| 1(구현 기록 UB1) | 2026-11-20 | UB1 구현(`Programs/Rocks.Build/Unity.cs`, `ActionGraph`의 번역 단위 구성, `NativeBuildEngine`의 묶음 파일 쓰기·정리, `Rocks.Tooling.Checks/PolicyChecks.Unity.cs`). 구현하며 정한 점. (1) 리플렉션 생성 소스는 기본으로 자기들끼리 묶는다(UD5·16절 개정 — GSD9 결정). 출력 디렉터리 아래 소스는 출력 기준 철자(`Generated/<M>/Source/…`)를 쓰고 그 묶음 TU에는 `/I<out>`을 맨 뒤에 붙인다 — 출력 폴더 이름이 묶음 구성과 내용에 섞이지 않는다. 루트와 출력 어느 쪽 아래도 아닌 소스는 묶지 않는다. (2) 모듈 규칙은 `Unity`(`UnityMode` Default·On·Off), `UnityBatchBytes`, `UnityAnchorFiles`, `UnityExclude`, `ReflectionUnity`다. `Default`는 빌드 기본값(`UnityBuild.DefaultOn`, 지금 끔)을 따르고 외부 라이브러리 모듈은 언제나 끔이다. 정리한 모듈은 규칙에서 `On`으로 켠다. (3) compile action의 Id에 변형을 넣었다(`<모듈>:compile:<변형>:<해시>`). 증분 기록을 Id로 찾으므로, 넣지 않으면 일반 빌드와 검증 빌드를 번갈아 돌릴 때 object는 남아도 기록이 서로를 덮어써 매번 전부 다시 컴파일한다. archive·link의 출력(`lib/`·`bin/`)은 변형이 공유하므로 변형을 바꾸면 그 둘은 다시 돈다. (4) "첫 `#include` 앞의 `#define`"은 그 뒤에 `#include`가 있을 때만이다. include가 없는 파일의 `#define`은 묶음 대상이고 `.cpp` 매크로 검사(`#undef`)가 본다. (5) 정리(6.4절): 묶음 디렉터리 `Unity/<모듈>/`은 유니티를 켠 빌드만 정리한다(검증 변형이 켠 빌드의 묶음 파일을 지우지 않는다). 변형 디렉터리를 쓰기 전 배치(`obj/<모듈>/` 바로 아래)의 파일도 지운다. (6) 도구 빌드 ID: archive·link의 object 인자는 원본 소스 키(`obj:<소스 키>`)의 ordinal 순 목록으로 적는다(묶음 object는 구성에 따라 개수가 바뀐다). compile 입력은 원본 소스만이다(묶음 파일 제외). (7) 정책 검사는 규칙 어셈블리를 평가해 유니티를 켠 모듈만 본다(플랫폼마다 따로 — 같은 플랫폼에서 함께 컴파일되는 소스끼리). 파일 지역 이름의 키는 "익명이 아닌 이름공간 경로 + 이름"이고, namespace 범위 `const` 변수도 파일 지역으로 본다(내부 링크다). 조사용 명령 `Rocks.ToolTests unity-audit`가 설정과 무관하게 모든 엔진 모듈의 충돌을 센다(2026-11-20: 1,970건 — EngineTests 764, FoundationTests 682, CoreFoundationTests 268, Sandbox 126, Crypto 82, 나머지 런타임 모듈 합 44). (8) CLI `--unity`·`--pch`·`--adaptive`를 모두 받는다(`--pch`는 변형 이름에만, `--adaptive`는 값 검사만 — 동작은 UB4). 빌드 결과는 묶음 수와 include 앞 `#define`으로 뺀 파일을 알린다. 확인: `Rocks.Build.Tests` 55묶음(새 `UnityTests` — 구성 결정성·국소성, `__FILE__`, 제외 규칙, 논리 명령, 유니티 켬·끔의 도구 빌드 ID manifest 동일, 변형 디렉터리, 정리, 리플렉션 생성 소스 묶기), 정책 검사 fixture(`Policy.UnityCollisionsRejectEachRule`) |
| 1(구현 기록 UB2) | 2026-11-20 | UB2 구현: 런타임·개발 모듈 40개의 유니티를 켰다(모듈당 커밋 하나, 규칙의 `Unity => UnityMode.On`). 충돌 130건은 파일 지역 이름을 `<파일 이름><이름>`으로 바꿔 정리했다(동작 불변 — Crypto 12개 파일, Core 6, Config 5, Serialization 4, PlatformBase 5 등). 구현하며 정한 점. (1) **검증 방법**: 단계마다 전체 빌드를 돌리지 않으므로(2026-11-20 사용자 결정) 모듈마다 "묶일 수 있는 소스 전부를 한 번역 단위로" 구문 검사(`-fsyntax-only`)를 했다 — 실제 묶음 구성보다 강한 조건이다(어느 두 파일이 만나도 된다). compile DB는 계획 전용 실행(`build --plan-only on` — 메타 생성과 계획까지만 하고 compile DB·묶음 파일을 쓴다. 이 단계에 더한 옵션)으로 얻었다. (2) **플랫폼**: 규칙의 `Unity = On`은 묶음을 검증한 플랫폼에서만 적용한다(`RepositoryUnity.VerifiedPlatforms` — Windows, Android). Android는 NDK 교차 빌드의 Foundation·EngineTests compile DB로 같은 검사를 통과했다. **macOS는 로컬 툴체인이 없어 끔 유지다** — macOS에서 같은 검사(또는 유니티를 켠 빌드)가 통과한 뒤 목록에 더한다(알려진 공백). 리플렉션 생성 소스 묶음은 플랫폼과 무관하게 켜져 있다. (3) OpenGLGraphics는 Windows에서만 켠다(Android 소스를 빌드하는 타깃의 compile DB를 만들 수 없었다). D3D11·D3D12·Dxgi는 Sandbox compile DB로 검사해 켰다. DeterministicMath는 켜지 않는다(object 단위 검사 `DeterministicMathObjectCheck`가 그 모듈 소스의 object를 따로 본다). Crypto의 AES 커널 두 파일(`AesX64.cpp`·`AesArm64.cpp`)은 `UnityExclude`다(아키텍처별 파생 코드이고 같은 커널 이름을 선언한다). (4) 정책 검사가 놓친 충돌 둘을 사전 검사가 잡았다: private 헤더의 inline 함수와 같은 이름의 `.cpp` 지역 함수(ObjectRuntime — 이름 변경), 익명 namespace의 `using` 선언과 다른 파일의 변수(Serialization — 정책 검사에 using 선언을 더했다). 같은 별칭·using 선언을 두 파일이 똑같이 적는 것은 충돌로 보지 않는다(재선언이 적법하다). (5) 프로그램 모듈(Sandbox 127건, 벤치, Commandlet)은 켜지 않았다 |
| 1(구현 기록 UB3) | 2026-11-20 | UB3 구현: 시험 실행 파일 모듈(CoreFoundationTests·FoundationTests·EngineTests — 규칙 하나)의 유니티를 켰다. 충돌 1,700건은 이름을 하나씩 바꾸는 대신 **시험 소스마다 파일 지역 부분을 파일 고유 namespace로 감쌌다**(`namespace <파일 이름 snake_case>_file { … }` — include와 `using namespace rocks;` 뒤부터 끝까지, 417개 파일, 기계 적용, 동작 불변). 익명 namespace의 도우미와 `TEST` 본문이 같은 namespace 안에 있으므로 다른 파일의 같은 이름 도우미가 한 묶음에서 만나도 이름 탐색이 자기 파일 것에서 끝난다(overload 집합이 섞이지 않는다). 이름 붙은 namespace 블록, `#include`, `extern "C"`, 다른 파일이 쓰는 함수 정의, 이름 붙은 namespace의 클래스 멤버 정의는 감싸지 않는다. `rocks` 밖의 파일 범위 `using namespace`(하위 namespace 포함 — `detail::`이 모호해진다)는 감싼 namespace 안으로 옮겼다. 전처리 조건 안에서 namespace를 여닫아야 하는 파일은 감싸지 않는다. 감쌀 수 없는 30개 파일(시험 타입에 대한 `rocks` 템플릿 특수화, host service 매크로, 러너가 부르는 probe)은 규칙의 `UnityStandaloneSources`(`UnityExclude`)로 단독 번역 단위다. 그 밖의 정리: `.cpp` 매크로 3개에 `#undef`, Windows 헤더 매크로와 겹친 지역 변수 이름 하나(`small`), 묶음에서 모호해지는 `detail::` 한정(Crypto 시험 3개 파일). 검증: 감싼 파일마다 단독 구문 검사, 시험 디렉터리마다 "전체 소스를 한 번역 단위로" 구문 검사(Windows 40개 디렉터리, Android 40개), 정책 검사 0건 |
| 1(구현 기록 UB4) | 2026-11-20 | UB4 구현(`Programs/Rocks.Build`의 `WorkingSet.cs`·`Pch.cs`, `ActionGraph`의 강제 include·PCH·분리, MetaCompiler `--definitions-header`). 구현하며 정한 점. (1) **정의 헤더**: `<out>/Definitions/<모듈>.Definitions.h`(머리말 + 이름 순 `#define 이름 값`, 값 없는 정의는 1). 모듈 정의(`ROCKS_MODULE_NAME` 포함)가 명령줄에서 빠지고 모든 번역 단위(C 포함)가 강제 include한다. 정의가 없는 모듈은 헤더가 없다. 정의 계산은 `ModuleDefinitions.Of` 하나이고 MetadataRunner도 같은 함수로 같은 파일을 써서 MetaCompiler에 `--definitions-header`로 넘긴다(MetaCompiler는 AST·탐침·검증 컴파일에 강제 include한다). (2) **모듈 PCH**: 규칙 `PrivatePchHeader`(모듈 폴더 기준 경로)·`PchUsage`·`PrivatePchMinTus`·`AdaptivePch`. PCH action의 종류는 `Pch`, 위치는 `<out>/Pch/<모듈>/<변형 key 12자리>/<모듈>.pch`(+ clang-cl stub `.Pch.cpp`·`.Pch.obj`, 의존 기록 `.pch.deps`)다. 변형 key에서 빼는 경로 인자는 include 디렉터리(`/I`·`-I`)다. PCH를 쓰지 않는 번역 단위는 C 소스, 생성 TU(BuildInfo·규칙 생성·모듈 표), 첫 include 앞에서 `#define`하는 파일이다 — 그 판정을 위해 유니티를 끈 모듈의 소스도 PCH를 쓸 수 있으면 훑는다. `UnityExclude`·큰 파일·리플렉션 생성 소스는 PCH를 쓴다. (3) **작업 파일**: `NativeBuildRequest.Adaptive`(라이브러리 기본 false, CLI 기본 on)면 엔진이 계획 전에 `SourceWorkingSet.QueryAsync`(루트의 작업 사본에서 `git status` 한 번)로 집합을 구한다. ProcessRunner가 유한한 제한만 받으므로 받는 최댓값(약 49일)을 넘겨 사실상 제한 없이 기다린다. 짝 헤더의 "모듈 안에서 유일"은 모듈의 public·private include 디렉터리와 소스 디렉터리의 헤더 이름으로 센다. (4) CI는 `-p:Adaptive=off`를 명시한다(`Rocks.proj`에 `Unity`·`Pch`·`Adaptive` 속성). (5) 이 단계에서는 어떤 엔진 모듈에도 `PrivatePchHeader`를 주지 않았다 — PCH 헤더 구성은 UB5의 공유 PCH에서 정한다. 확인: `Rocks.Build.Tests` 58묶음(새 `PchTests` — 정의 헤더, 모듈 PCH 생성·사용·같은 크기 헤더 변경·하한·`--pch off`·제외·유니티 병용·낡은 변형 정리, porcelain fixture·분리 규칙·분리한 파일만 재컴파일·도구 빌드 ID 불변·실제 git), 도구 시험 `Meta.DefinitionsHeaderIsForceIncluded` |
| 1(구현 기록 UB5) | 2026-11-20 | UB5 구현(`Pch.cs`의 공유 PCH 계획과 `SharedPchChecks`, `TimingReport.cs`, SDK manifest 9). 구현하며 정한 점. (1) **공유 PCH action은 제공 모듈의 public include closure로 컴파일한다**(소비 모듈의 include 목록이 아니다). 그래서 같은 변형 key의 소비 모듈들이 정확히 같은 action 하나를 함께 쓰고, 어느 소비 모듈이 먼저 나오느냐에 따라 PCH가 달라지지 않는다. 포함 검사(그 closure ⊆ 소비 모듈 include 목록)는 계획 때 한다. (2) 제공 모듈 자신도 후보다(모듈 PCH가 없으면 자기 공유 PCH를 쓴다). (3) 공유 정의 헤더는 `<out>/Definitions/<제공 모듈>.Shared.Definitions.h`, 소비 모듈의 추가 정의 헤더는 `<모듈>.Extra.Definitions.h`다. PCH를 쓰지 않는 번역 단위(C 소스 등)는 지금처럼 모듈 정의 헤더 전체를 받는다(같은 매크로 집합이다). (4) **shadow·추가 정의 검사는 소비 번역 단위를 실행하기 직전에 한다**(`NativeAction.PreRun` — PCH의 의존 기록이 PCH 헤더 closure이고 그 기록은 PCH action이 끝나야 있다). 모듈마다 한 번 계산하고, 위반이면 그 모듈의 소비 action이 도구를 실행하지 않고 실패한다(오류 문구에 정의 이름·헤더·줄, 간접 참조면 거친 매크로). cache hit인 action은 검사하지 않는다. shadow 검사의 "철자"는 제공 모듈 include 디렉터리 기준 상대 경로로 본다. (5) SDK manifest는 제공 모듈의 `SharedPchHeader` 철자만 싣는다 — 공유 정의는 이미 실린 public define에서 다시 계산한다(게임 빌드가 자기 인자로 PCH를 만든다). (6) `--timing on`: compile·PCH action에 `-ftime-trace=<출력>`을 붙이고(trace는 action 출력 `<object>.time.json`) 성공한 빌드 끝에 `<out>/Timing/<타깃>.timing.json`(번역 단위·PCH action별 시간, 헤더·템플릿 상위 100)으로 집계한다. clang 기본 세밀도(500 µs)보다 짧은 항목은 trace에 없다. (7) **엔진 모듈에는 아직 `SharedPchHeader`를 주지 않았다.** 헤더 구성은 `--timing` 집계로 정하기로 했는데(8.4·12절), 측정용 빌드를 돌리지 않기로 한 결정(2026-11-20)과 맞물려 이번 작업에서는 기구와 시험만 넣었다. Core·ObjectRuntime·Engine의 공유 PCH 헤더는 `--timing on` 빌드 한 번의 집계를 보고 정한다(남은 일). 확인: `Rocks.Build.Tests` 59묶음(새 공유 PCH 묶음 — 제공 모듈 선택, 공유·추가 정의 헤더와 강제 include 순서, 변형, shadow 위반, 추가 정의 직접·간접 위반, timing 집계) |
| 1(구현 기록 UB5 후속) | 2026-11-20 | 엔진 모듈에 공유 PCH 제공 모듈을 정했다(사용자 요청 — 제공 모듈이 없으면 공유 PCH 기구가 아무 일도 하지 않는다). Core `Core/CoreSharedPch.h`(26개)와 ObjectRuntime `Object/ObjectSharedPch.h`(Core 목록 + 15개)이고, 고른 방법·기준·목록·넣지 않은 것(`Object/Object.h` 계열, Engine·Graphics 제공 모듈)은 8.4절에 적었다. 의존 기록 대신 include 그래프 정적 분석과 4주 커밋 수를 썼다(이 작업 트리에 UB0 뒤 빌드 출력이 없었다). 함께 고친 것: 공개 헤더 단독 컴파일 검사 모듈은 유니티·PCH를 끈다(10절 — 빠져 있었다), 정책 검사 `pch-header-include`(10절), `Rocks.proj`의 `-p:Timing`. 시험: `Rocks.Build.Tests`의 PCH 묶음이 NDK가 있으면 같은 fixture(정의 헤더·모듈 PCH·공유 PCH)를 clang++로 교차 빌드한다(GNU 드라이버의 `-x c++-header`·`-include-pch` 경로 — macOS도 이 경로다), `--group dependency|unity|pch`로 묶음 하나만 돌릴 수 있다. 확인: PCH 묶음 5개 PASS(NDK 포함), 정책 검사 통과와 `Policy.PchHeaderIncludesAreRejected`, EngineTests·Foundation·Sandbox·Commandlet 계획 생성(변형은 제공 모듈마다 1개), 고친 모듈·시험 디렉터리의 한 TU 문법 검사(공유 PCH 헤더를 강제 include한 논리 명령으로) |
| 1(구현 기록 UB6) | 2026-11-20 | 기본값과 UD20을 정했다. 반복 측정 없이 권장안으로 정한다는 사용자 결정에 따랐다. (1) **기본값.** CLI `--unity`·`--pch`·`--adaptive`는 모두 on이고 CI는 `--adaptive off`다(11절 그대로). 손으로 쓴 소스의 유니티는 규칙에서 켠 모듈만 묶는다(`UnityBuild.DefaultOn = false`) — 켠 모듈은 묶음 충돌 정책 검사의 대상이므로 정리해 통과한 모듈만 켠다. 지금 켜진 것은 엔진 모듈 40개와 시험 실행 파일이고, 꺼 둔 것은 Sandbox·Commandlet·벤치 프로그램·DeterministicMath다(게임 모듈도 기본은 끔이다). 리플렉션 생성 소스는 기본으로 묶는다(GSD9). `UnityAnchorFiles` 16, `UnityBatchBytes` 384 KiB, `PrivatePchMinTus` 3은 그대로다. (2) **UD20.** `-fpch-codegen`·`-fpch-debuginfo`는 쓰지 않는다 — PCH object를 링크해야 하고 정적 라이브러리 경계와 얽히는데, 엔진 헤더의 파싱·inline 코드 생성 몫이 작아(8.4절 실측) 얻을 것이 적다. 메모리 기반 병렬도 제한은 두지 않는다(13절). C++ modules는 도입하지 않는다. (3) **하지 않은 것.** `unity-profile`(12절 — 계측 기구가 필요하고 결과를 쓸 측정 빌드가 없다)과 기준 시나리오 20회 반복 측정(사용자 결정). 대신 끝의 검증 빌드 한 번의 수치를 이 이력에 적는다. (4) **CI.** Windows에서 검증 변형(`/p:Unity=off /p:Pch=off`) Debug 빌드를 한 번 돈다(UD14 — 링크까지, 시험은 돌리지 않는다). 기본 변형과 같은 출력 디렉터리의 다른 object 변형 경로(`obj/<모듈>/u0p0/`)를 쓰며 모든 모듈을 다시 컴파일한다. (5) **남은 일.** `unity-profile`과 반복 측정, macOS에서 유니티 켜기(CI에서 한 번 확인한 뒤 `RepositoryUnity.VerifiedPlatforms`에 더한다 — 9.3절), Engine 공유 PCH와 `Object/Object.h` 계열의 PCH 편입(8.4절), 작업 파일 분리와 PCH를 켠 채로 I3(private `.cpp` 하나) 반복 시간 측정 |
| 1(검증 기록) | 2026-11-20 | 끝에 한 번 돌린 검증의 결과다(Windows, Debug, `--adaptive off --timing on`, 8 병렬, 다른 세션의 빌드로 CPU가 포화된 장비). **결과.** Foundation: 746 action(compile 711·PCH 2·archive 32·link 1), 시험 2,549개 실패 0, 도구 시험 self 148·native 124·meta 246 통과, 무변경 재빌드 0 action. EngineTests: 887 action(compile 834·PCH 2·archive 50·link 1), 시험 2,954개 실패 0, self 148·native 42·meta 246 통과, 무변경 재빌드 0 action. `Rocks.Build.Tests` 61묶음, 통합 시험(설치 SDK 게임 포함), format, policy(묶음 충돌·`pch-header-include` 포함) 통과. 검증 변형(유니티·PCH 끔)의 Foundation Debug 빌드도 통과했다(1,337 action — 소스마다 단독 번역 단위). Android(NDK)는 빌드하지 않고, EngineTests 계획이 쓴 실제 묶음 파일 103개를 공유 PCH 헤더를 강제 include한 논리 명령으로 문법 검사해 통과했다(clang++ PCH 생성·사용은 `Rocks.Build.Tests`의 교차 빌드 fixture가 본다). **검증에서 고친 것.** (1) 첫 Foundation 빌드에서 시험 실행 파일의 묶음 3개가 `detail`이 모호하다는 오류로 실패했다. 원인은 둘이다 — `TestSupport/Private/Win64`의 시험 4개가 감싸기 대상(`Tests/` 디렉터리)에서 빠져 파일 범위 `using namespace rocks::platform;`이 묶음의 다음 파일로 샜고, 하위 namespace using에 기대어 `detail::`을 쓰던 시험 3개가 공유 PCH와 다른 디렉터리의 구성원이 들여온 `rocks::detail` 때문에 모호해졌다. 4개를 파일 고유 namespace로 감싸고 3개는 `platform::detail::`·`deterministic::detail::`로 한정했다. 시험 모듈의 사전 검사는 디렉터리 단위였는데 실제 묶음은 경로 순서로 디렉터리를 넘는다 — 사전 검사는 실제 묶음 파일로 해야 한다. (2) `--timing` 집계의 헤더 목록이 비어 있었다. clang은 `Source`(파일 하나의 파싱)를 길이가 아니라 시작·끝 쌍(`ph` `b`·`e`)으로 쓴다. 쌍의 시각 차를 더하고 묶음 구성원 소스는 헤더 목록에서 빼게 고쳤으며, 손으로 만든 trace로 시험한다(UB5 때 "세밀도보다 짧아서"라고 본 것은 틀린 진단이었다). clang-cl `/Yc`는 PCH 생성과 stub 컴파일 두 단계로 돌고 trace는 뒤 단계 것만 남으므로, PCH action의 시간은 trace가 아니라 action 소요 시간으로 본다(검증에서 0.4–0.8 s). (3) 통합 시험 두 곳이 낡아 있었다. SDK 게임 빌드는 SDK가 나른 공유 PCH 헤더를 게임 출력에 한 번 미리 컴파일하므로(모듈 이름이 제공 모듈인 Pch action) "게임 모듈의 action만 있다"는 확인을 그에 맞췄고, 필드를 더한 뒤의 게시는 타입별 생성 헤더가 아니라 생성 소스와 descriptor로 확인한다(GS2 뒤 헤더는 그대로다). C# 서식 위반 두 곳도 고쳤다. **수치**(Foundation clean build, 같은 장비 — 부하가 달라 벽시계는 거친 비교다). 이전(GS0 전: 생성 소스 분리·유니티·PCH 없음): 1,278 action, compile 시간 합 5,799.5 s(시험 실행 파일 386 TU 3,970.6 s, 공개 헤더 검사 401 TU 693.6 s), 메타 생성 229.5 s + native 776.5 s. 생성 소스 분리만(검증 변형 — 유니티·PCH 끔): 1,337 action, compile 시간 합 752.8 s(시험 실행 파일 390 TU 262.1 s, 공개 헤더 검사 405 TU 151.0 s), 메타 단계 72.8 s + native 103.3 s. 기본 변형(유니티·PCH 켬): 746 action, compile 시간 합 529.6 s(시험 실행 파일 59 TU 133.0 s, 공개 헤더 검사 405 TU 154.4 s), 도구 빌드·메타 생성·compile·archive까지 138 s(link 전에 위 묶음 3개가 실패한 첫 실행이고 장비 부하가 더 컸다). compile 시간 합으로 보면 생성 소스 분리(GS0–GS2)가 7.7분의 1로 줄였고, 유니티와 PCH가 거기서 30%를 더 줄였다(시험 실행 파일은 절반). 유니티의 나머지 이득은 프로세스 수(1,304 → 711)와 link 입력 수다. EngineTests clean build: 887 action, compile 시간 합 1,003.8 s(공개 헤더 검사 488 TU 484.0 s, 시험 실행 파일 70 TU 143.2 s, Sodium 142 TU 123.0 s), PCH action 2개 1.6 s. **UD20 확인.** 8 병렬의 두 빌드에서 메모리 부족이 없었다 — 메모리 기반 병렬도 제한은 두지 않는다(13절). **돌리지 않은 것.** Release·ASan·Development 구성, macOS(로컬 툴체인 없음 — 유니티는 꺼져 있고 PCH·감싼 시험·바꾼 이름은 macOS에서 컴파일해 보지 않았다), Android 전체 빌드, Sandbox·Commandlet·벤치 타깃 빌드(계획 생성까지만), 12절의 반복 측정 |
| 1(경로 철자) | 2026-11-20 | 4.4절을 더했다: 빌드가 받은 경로를 디스크에 있는 철자로 맞춘다(`PathSpelling`). 같은 트리를 `c:\`와 `C:\`처럼 철자만 다르게 실행하면 전부 다시 컴파일되던 문제다. UE `FindCorrectCase`와 같은 방식(성분마다 부모 디렉터리에서 이름을 찾음)이고 링크는 풀지 않는다. |
| 1(macOS 유니티) | 2026-11-20 | macOS에서도 유니티 빌드를 켰다(`RepositoryUnity.VerifiedPlatforms`에 macOS). POSIX 공통 소스는 NDK 묶음 문법 검사로 먼저 보고, macOS 전용 소스(PlatformBase의 `Mac`·`Apple`, ShaderToolLoader의 `Mac`)는 CI macOS job으로 확인했다. 로컬 macOS 툴체인이 없으므로 macOS 전용 소스를 고치면 CI로 확인한다 |
| 1(매크로 누출) | 2026-11-21 | 6.1절에 "매크로를 남기는 헤더"의 규칙을 더했다: Windows SDK 계열 헤더는 `Platform/Win64/WindowsHeadersBegin.h`/`WindowsHeadersEnd.h` 쌍으로 감싼다. Begin이 `Windows.h`를 열고 A/W 함수 이름 매크로를 지운 뒤 앞서 숨긴 `near`·`far`·`small`·`interface`·`min`·`max`를 되돌리고, End가 다시 숨긴다. 정책 검사 `unity-macro-leak`(9.2절)이 감싸지 않은 include를 거부하고, `platform-boundary`는 이 쌍을 `#pragma once` 없는 조각으로 받는다. 유니티를 켠 모듈의 기존 Windows 소스·헤더 39개를 감쌌고, 누출 때문에 단독이던 `WindowsComRefTests.cpp`를 묶음으로 되돌렸다. 쌍 자체의 시험은 `TestSupport/Private/Win64/WindowsHeadersTests.cpp`다(동작은 그대로다. `CrashFiles.cpp`의 `FILE_DISPOSITION_INFO::DeleteFile` 대입 두 곳은 이름 없는 초기화로 바꿨다) |
| 1(매크로 누출 리뷰 반영) | 2026-11-21 | 6.1절 "매크로를 남기는 헤더"를 고쳤다. (1) 규칙과 정책 검사의 범위를 유니티를 켠 모듈에서 Windows 플랫폼의 모든 모듈로 넓혔다(숨긴 뒤 쌍 밖에서 SDK 헤더를 여는 `ShaderToolSpike` 빌드가 깨졌다). 정책 이름은 `windows-macro-leak`이다. (2) 쌍이 `push_macro`/`pop_macro` 대신 정의를 직접 다시 쓴다(clang PCH가 push 스택을 저장하지 않는다). (3) 정책이 이름 목록 대신 깨끗한 시스템 헤더 목록으로 판정하고, 외부 라이브러리 헤더를 따라가며, 줄 이음과 리터럴이 아닌 include를 다룬다. Windows 플랫폼만 평가하고 파일 해석을 캐시한다. (4) 숨기는 이름에 `pascal`·`cdecl`·`hyper`·`RegisterClass`·`FindFirstFile`을 더했다. (5) UE 대응 서술을 바로잡았다(A/W 이름만 같다) |
| 1(placement new 헤더) | 2026-11-21 | 8.4절에 "placement new 헤더"를 더했다: `Base/PlacementNew.h`가 MSVC 런타임에서는 `<vcruntime_new.h>`를, 그 밖에서는 `<new>`를 include하고, 엔진 소스는 `<new>` 대신 이 헤더를 쓴다(정책 `placement-new-header`, 9.2절). 공개 헤더 단독 컴파일 검사와 PCH를 쓰지 않는 번역 단위에서 `<new>` 사슬(번역 단위당 0.126 s)이 빠진다 |
