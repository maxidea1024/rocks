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

상태: **설계 확정**(결정 UD1–UD20, 2026-11-18 사용자 승인 — 권장안). 리뷰어 교차 검증을 반영했다. 구현 단계는 UB0–UB6(15절)이며 착수 시점은 사용자가 정한다.

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
- 규칙이 단독으로 지정한 파일(`UnityExclude`). 큰 파일이나 특수 플래그 파일이다.
- **첫 `#include` 앞에서 `#define`하는 파일**(예: `Core/Tests/StlCompatibilityTests.cpp`의 `_HAS_EXCEPTIONS 0`). 같은 묶음의 앞 파일이나 PCH가 그 헤더를 이미 처리했으면 설정이 조용히 무효가 된다. 빌드가 토큰 단위로 찾아내 단독 TU로 컴파일하고 PCH도 끈다. 빌드 보고에 파일 목록을 남긴다.
- 상한보다 큰 파일. 단독 TU다.

모듈 규칙 `Unity`가 `Off`면 묶지 않는다. 외부 라이브러리 모듈(`Core/Source/ThirdParty`)의 기본은 `Off`다. upstream이 유니티를 전제하지 않는다.

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
- 초기 제공 후보는 Core, ObjectRuntime, Engine이다. 헤더 구성은 UB5에서 `--timing` 집계(12절)로 정한다.

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

- 검사는 토큰 단위 근사다. 주석, 문자열, 전처리 조건을 처리하지만 컴파일러 AST 수준의 정확성은 요구하지 않는다. 놓친 것은 드러나는 경우 컴파일 오류로 남는다.
- 이름 충돌은 파일마다 고유한 이름의 namespace로 옮기거나(예: `namespace rocks::detail::JsonReaderLocal`) 이름을 바꿔서 푼다. 익명 namespace 자체는 금지하지 않는다.

### 9.3 정리 범위

1. 런타임 모듈(356개)을 먼저 정리하고 유니티를 켠다(UB2).
2. 시험 실행 파일 모듈(FoundationTests·EngineTests, 373개)을 정리하고 켠다(UB3). 그 전까지 시험 모듈은 `Unity = Off`다.

정리 커밋에는 동작이 바뀌지 않는 이동과 이름 변경만 담는다.

## 10. include 위생

- `.cpp`의 첫 include는 짝 헤더(있으면)다. 작업 파일 분리(7.2절)가 이 규칙을 쓰고, 정책 검사가 확인한다(UE `bEnforceIWYU`, `UEBuildModuleCPP.cs:2427–2475`).
- 소스는 PCH 헤더와 정의 헤더를 직접 include하지 않는다(8.1절). 정책 검사가 확인한다.
- 공개 헤더 단독 컴파일 검사(`CheckPublicHeaders`)는 PCH와 유니티 없이 돈다.
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

## 14. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| UD1 | 정밀 헤더 의존(2단 key: 선언 입력 key + 의존 기록 검증, include 디렉터리 이름 목록, 툴체인 소유 헤더 제외)을 유니티·PCH 앞 단계(UB0)로 둔다 | 확정 |
| UD2 | compile identity에서 모듈 전체 소스 목록을 빼고 TU 구성만 넣는다 | 확정 |
| UD3 | 큰 산출물 검증은 (크기, 수정 시각, 파일 ID)가 모두 같으면 기록 해시를 쓰고, 다르면 다시 해시한다 | 확정 |
| UD4 | 묶음 경계는 전체 목록 기준 경로 해시 anchor(고정 간격 16)로 구간을 나누고, 구간이 상한(384 KiB, 실제 바이트)을 넘으면 파일 수로 균등 분할한다. 구성원 하나면 단독 TU | 확정 |
| UD5 | BuildInfo·규칙 생성 TU, C 소스, 외부 라이브러리 모듈은 묶지 않는다 | 확정 |
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
| UD20 | `-fpch-codegen`·`-fpch-debuginfo`, 메모리 기반 병렬도 제한, C++ modules는 UB6 측정 뒤 별도로 결정한다 | 보류 |

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
| 생성 코드 묶기 | `.gen.cpp`를 앞에 두고 4 KB placeholder로 셈 | 생성 TU는 묶지 않는다 | 리플렉션 생성물이 헤더뿐이다 |
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
