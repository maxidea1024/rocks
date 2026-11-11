# Rocks Foundation 최초 구현 상태

기준일: 2026-11-10. 이 문서는 **Core·Object의 첫 실행 가능한 구현 범위**를 기록한다. C00–C13 전체 완료, Foundation Gate 통과, 제품용 엔진 완성을 뜻하지 않는다. 설계 PDF와 작업 CSV는 목표 기준이며 실제 진척은 이 문서와 실행 증거를 함께 확인한다.

현재 소스는 Name·Text·플랫폼 기반 위에 worker 실행기, 점진·병렬 GC, Clang 생성 리플렉션과 객체 그래프 직렬화, C#/.NET 기반 빌드 도구를 포함한다. 아래 실행 기록과 [5차 검증 기록](../Reviews/ImplementationRound5.FoundationValidation.ko.md)은 각각 해당 당시 소스에 대한 결과다. 현재 Windows 검증은 아래에 별도로 기록하며, 과거 CI를 현재 소스나 macOS의 통과 근거로 사용하지 않는다.

신규 Stack·Binned·가상 메모리 코드는 테스트와 하네스를 포함한다. 이번 변경분의 Windows Core Debug·Release 및 Foundation Debug·ASan 검증은 통과하였으며, macOS 빌드·실행은 미검증이다. 가상 메모리의 OS 실패·부분 변경 fault injection은 보강해야 한다. Windows에서 mimalloc v3.6.1을 별도 scratch probe로 연결한 결과는 통과하였으나 제품 빌드·배포 통합이나 macOS 검증은 아니다. jemalloc·TCMalloc의 실제 제품 연결 및 성능은 미검증이다.

이번 Windows 검증에서 Core Debug·Release는 각각 PASS였고, Core Release는 native 211건과 harness 50건을 실행하였다. Foundation Debug는 native 298건·harness 57건·meta 41건, Foundation ASan은 native 298건·harness 58건·meta 41건이 PASS였다. build evidence는 실행 중 source가 안정적이고 runner report가 현재 소스에 해당함을 기록한다. 이 결과는 제품 성능, 외부 공급자 제품 통합 또는 macOS 실행을 보증하지 않는다.

## 1. 현재 동작하는 경로

`Base → PlatformBase → Core → MetaSchema/Task → ObjectRuntime`을 기반으로 `Serialization → Core`, `ObjectSerialization → ObjectRuntime + Serialization`을 연결한다. CPU 전용 테스트 호스트에서 실제 worker, 생성 리플렉션, 객체 그래프 저장·복원과 비이동 점진·병렬 GC를 실행한다. 기존 InlineExecutor와 full STW collector는 기준 경로로 유지한다. 객체 변경은 단일 소유 스레드이며 병렬 marker와 mutator를 동시에 실행하지 않는다.

엔진 소유 Runtime과 공개 API에는 STL을 사용하지 않는다. C++23, 예외 비활성, RTTI 비활성, 공개 헤더 단독 컴파일, 경고를 오류로 처리하는 설정을 사용한다. OS/컴파일러 기반 기능은 Platform과 compiler intrinsic 경계로 제한한다.

## 직관적 API의 현재 상태

직관적인 사용법과 오류·수명 안전성은 전체 계획의 공통 요구다. 현재 `TypeRegistry::IsA()`는 `Result<bool>`을 반환하므로 조건문의 성공 여부와 상속 여부가 혼동될 수 있어 후속 개선이 필요하다. 현재 생성 헤더를 포함한 타입은 `TypeIdOf<T>()`와 `TypeOf<T>()`로 정적 ID와 schema를 얻고, 유효한 객체 Borrow에서는 `GetType()` 또는 `TryGetType()`으로 실제 등록 타입 lease를 얻는다. lease의 `Id()`는 값이며 `Name()`·`Schema()`·`Ops()`의 뷰와 참조는 lease 수명에 종속된다. `T::StaticType()` 멤버와 클래스 자체를 데이터로 다루는 전체 메타클래스 기능은 별도 후속 범위다. 새 코드에 대한 플랫폼별 실행 결과는 과거 검증 기록과 구분한다.

## 런타임 클래스 기능의 현재 범위

현재 타입 메타데이터와 `TypeLease`, `TypeRegistry`는 클래스 정보와 수명을 관리하는 기초다. 등록된 `TypeId`는 `ObjectRegistry::NewObject<Object>(access, typeId)`를 통해 낮은 수준의 런타임 객체 생성 경로에 전달할 수 있다. 타입 제한 클래스 참조, 클래스 기본 객체, 함수 반영, 에디터 클래스 선택과 모듈 reload 대응을 포함한 전체 메타클래스 기능은 별도 후속 범위다. 요구·수명 규약·검증 계획은 [런타임 클래스 메타데이터 설계](../Design/RuntimeClassMetadata.ko.md)에 정리한다.

## 2. 모듈·파일과 구현 범위

| 작업 | 현재 구현 | 아직 구현하지 않은 범위 |
|---|---|---|
| C00 | .NET 10 SDK 고정, C# `*.Build.cs`/`*.Target.cs` 모듈·타깃 규칙, MSBuild 진입점, 네이티브 작업 그래프·로컬 증분 검사, 설치 SDK·외부 게임 빌드, C# 검사 하네스·서식/STL/경고 검사, 단독 헤더 검사, 모듈 템플릿 plan/apply/validate, CI 정의 | 분산 빌드·공유 artifact 캐시, PCH·unity·공유 라이브러리, 완전한 SDK 패키징, 전체 프로젝트·컴포넌트·프리팹 템플릿, 복구 저널 |
| C01 | Base 정수 타입, Guid POD, checked arithmetic | 전체 ABI·compiler 추상화 |
| C02 | 정렬 할당, 가상 메모리 예약·commit·decommit·release, 단조 시계, 스레드 식별, Thread/Mutex/CV/Event/Semaphore, 최소 fatal sink | 가상 메모리 OS 실패·부분 변경 fault injection과 macOS 실행 검증, atomics/RWLock, thread 설정·취소 |
| C03 | Result/Error, Move/Forward, 제한된 traits | 전체 범용 utility 집합 |
| C04 | SystemAllocator·ArenaAllocator·StackAllocator·PoolAllocator·BinnedAllocator·BudgetAllocator·DebugAllocator, 외부 backend와 벤더 함수 바인딩, 소유권 ledger·통계 | frame 수명 관리·자동 일괄 해제 stack·thread cache·page guard, 실제 공급자 제품 통합·플랫폼 검증, 주소 검색 최적화·성능 측정 |
| C05 | Array, Span, HashMap, HashSet, 찾거나 추가·조건부 삭제·집합 연산, 실패 시 상태 유지, 비소유 뷰 규약 | queue, inline·bit·stable-slot 컨테이너, 범위 연산, 내부 콜백 재진입 완전 진단 및 장시간 성능 검증 |
| C06 | Function, ModuleLease, InlineExecutor와 WorkerScheduler, 의존성·취소·drain, 고정 용량 로그·프로파일 scope, assert, 음성 대조군 | priority/lane/work-stealing/fiber scheduler, 비동기 로그 writer·viewer |
| C07 | C# Clang AST 메타 생성·선언 검증, CLASS/PROPERTY·typed 필드 접근·자동 Trace, 상속 metadata IsA, 깊은 복사·revision/module lease, Archive/Record·객체 그래프 직렬화 | 자동 메타 unit 발견·프로젝트 전역 ID 검증, 범용 native 상속 cast, method/enum/container reflection, compile DB 입력 재구성·생성 캐시·외부 에셋 참조 |
| C08 | 비이동 64-slot chunk, registry domain·generation, 고갈 슬롯 격리, 진단 캡처 | 빠른 인덱스 조회, 대규모 벤치마크 |
| C09 | RootRef/WeakRef/ObjectRef/Borrow/BackendLease, 문맥·스레드 검사 | root owner chain, 전체 safepoint 시스템 |
| C10 | 비공개 생성 트랜잭션, 간선 검증 후 일괄 공개, 역순 롤백 | ClassDefaults·instancing, 트랜잭션 활성 중 GC, 전체 lifecycle hook |
| C11 | full STW와 점진 mark/sweep, 삽입 장벽·cutoff, 선택적 병렬 mark, soft 시간·작업량 예산, OOM 시 sweep 금지·독립 oracle | 다중 mutator·동시 필드 스캔, hard deadline, 병렬 sweep·cluster·대규모 성능 검증 |
| C12 | logical destroy·retire·실제 회수 분리, Busy 종료·재시도, 모의 backend lease | 실제 GPU/IO/audio 연동, 장시간 시스템 스트레스 |
| C13 subset | 자체 UTF-8 String/StringView, NamePool/NameLease intern, 명시 수명·예산·경합 검증 | Guid 생성/parse, 암호학적·영속 hash 정책, 직관적인 자체 Path 값·플랫폼 변환 |

플랫폼의 단조 시계와 WorkerScheduler는 시간 측정 및 작업 실행 기반이다. 게임·엔진의 가변 틱, 고정 틱, 시간 배율, 틱 그룹과 멀티스레드 틱은 아직 구현하지 않았다. Component의 생성·부착·소유·수명 시스템과 프리팹, 에디터·PIE도 미구현이다. `Templates/Game/Foundation`은 현재 Object/CLASS 예제이며 Component 예제는 실제 시스템 구현 이후 추가한다.

빌드 도구는 .NET 10과 MSBuild로 호스트 도구 및 규칙을 구성하고, `Rocks.Build`가 C++ 컴파일·정적 라이브러리·실행 파일의 의존성과 로컬 증분 판단을 수행한다. 설치 SDK에는 헤더·빌드된 라이브러리·호스트 도구와 호환성 manifest가 포함되며 외부 게임은 별도 경로에서 빌드한다. 현재 SDK 내보내기는 임의 컴파일·링크 옵션이나 외부 사전 빌드 라이브러리가 있는 모듈을 지원하지 않고 명시적으로 거부한다. CMake/Python 파일은 이전 구현의 참고 경로이며 새 빌드·검증의 기준은 .NET 경로다.

메타컴파일러는 각 타깃의 `TargetRules.Metadata`에 수동 등록한 헤더 단위로 Clang AST를 분석하고, 허용된 대상 컴파일 조건을 사용해 생성 C++도 검사한다. 등록된 unit의 소유 모듈·중복 입력과 생성 출력, 타깃의 단일 ID 명세 및 여러 unit의 활성 선언 누락·중복을 검사한다. 같은 reflected 선언을 여러 unit에서 사용하는 구성은 현재 거부한다. 미등록 헤더 자동 발견과 서로 다른 타깃·명세를 통합한 전역 ID 검사는 아직 제공하지 않는다. 사용자 정의 전체 컴파일 옵션·완전한 compilation database 재구성·생성 캐시도 아직 제공하지 않는다.

별도 `Localization → Core` 모듈에서 Text 불변 이력·catalog snapshot·named format을 구현하였다. 지역별 plural/날짜/통화·추출·쿠킹·UI는 후속이다. [Name·Text 구현 계약](NameTextImplementation.ko.md), [플랫폼 독립 동시성](PlatformConcurrency.ko.md), [컴파일러 경고 정책](../Design/CompilerDiagnostics.ko.md)을 함께 따른다.

새 구현의 구체적 계약은 [WorkerScheduler](WorkerScheduler.ko.md), [점진·병렬 GC](IncrementalGarbageCollection.ko.md), [Reflection·메타 생성](ReflectionMetadata.ko.md), [직렬화](Serialization.ko.md)를 따른다. 이들 실행 경로의 구현 완료와 전체 Core 작업 목록의 완료는 구분한다.

주요 파일 위치는 다음과 같다.

- `Engine/Core/Public/Core/Memory`, `Containers`, `Text`: 메모리·컨테이너·문자열 계약.
- `Engine/Core/Public/Core/Function.h`, `Lifetime`, `Diagnostics`: callable·코드 수명·시스템 진단.
- `Engine/ObjectRuntime/Public/Object`: 타입 등록·핸들·참조·생성·GC 공개 API.
- `Engine/ObjectRuntime/Private`: 스키마 저장소, 비이동 Registry와 수집·회수 구현.
- `Tests/Core`, `Tests/Platform`, `Tests/Object`, `Tests/Foundation`, `Tests/Integration`: 단위·통합·장애·음성 대조군.
- `Rocks.proj`, `Build/Rules`, `Tools/Rocks.Build*`: MSBuild 진입점과 C# 규칙·네이티브 빌드·SDK 도구.
- `Tools/Rocks.MetaCompiler`, `Tools/Rocks.ToolTests`: C# 메타컴파일러와 검사·회귀 하네스.

## 3. 반드시 지켜야 할 수명·동시성 계약

1. Allocator → TypeRegistry → ObjectRegistry 순으로 생성하고 반대로 정리한다. 각 wrapper/뷰는 소유자가 살아 있는 동안만 사용할 수 있다. 종료 실패를 무시하고 먼저 파괴하면 강제 해제하지 않고 fatal로 드러낸다.
2. 객체 접근·생성·변경·GC는 Registry 생성 스레드 전용이다. gate는 상태와 계수를 보호하며 객체 필드까지 스레드 안전하게 만들지는 않는다. 서로 다른 RootRef의 Clone/Reset, 반환할 Borrow/BackendLease의 Reset은 정해진 범위에서 다른 스레드에도 허용한다. 동일 wrapper를 동시에 변경하지 않는다.
3. RootRef는 외부 GC 루트, ObjectRef는 소유 객체 안의 강한 간선, WeakRef는 비소유 값 핸들이다. Borrow는 제한된 포인터 접근권이며 하나라도 활성 상태면 Collect와 Begin/StepCollection이 Busy를 반환한다. BackendLease는 해당 저장소 회수만 지연하며 루트나 자식 접근권은 아니다.
4. Object와 ObjectRef의 복사·이동은 금지한다. ObjectRef는 SetReference를 통해 변경하고 소유자의 Trace가 빠짐없이 방문해야 한다. 현재는 객체 안에 직접 포함된 필드만 지원한다. 생성 메타데이터는 지원하는 전체 필드와 자동 Trace를 대조하지만, 수동 Trace 누락과 임의 raw pointer 오용까지 증명하지 않는다.
5. RequestDestroy는 논리적 접근을 닫는다. 외부 RootRef에서 도달 가능한 동안 파괴 요청 객체와 그 강한 간선의 자식을 보존하며, 도달 불가능한 cycle은 수거한다. 기존 RootRef의 Clone과 새로운 WeakRef pin은 다르게 처리한다.
6. ConstructionContext는 전체 공개 전까지 객체를 격리한다. 실패 가능한 초기화는 호출자가 결과를 확인하고 Commit을 생략한다. 첫 staged 객체만 루트로 반환되므로 나머지는 그 객체에서 도달하거나 별도 루트가 있어야 한다.
7. GC는 작업 큐를 먼저 확보한다. OOM이나 잘못된 간선으로 mark가 끝나지 않으면 sweep하지 않는다. 사용자 생성자·Trace·소멸자와 할당기 callback은 Registry 잠금 밖에서 실행한다. 변경 재진입은 거부하며 활성 생성 트랜잭션의 재진입 롤백은 금지한다.
8. Shutdown은 새 소유권 유입을 닫고 회수를 시도한다. 보유권이 남으면 Busy이며 반환 후 재시도한다. 접근 검증이나 processing 재진입 거부는 닫기 전에 반환한다. 닫은 뒤의 회수 실패는 닫힌 상태를 유지한다.

각 계약은 해당 공개 헤더와 복잡한 처리 단계에 한국어 주석으로도 남긴다. [코드 스타일](../CodingStyle.md)은 주석의 정확성과 갱신을 리뷰 기준으로 요구한다.

## 4. 문자열과 표시 문구

`std::string`이나 `std::wstring`을 도입하지 않는다. `String`은 Allocator 기반 UTF-8 소유 값, `StringView`는 길이 기반 비소유 뷰다. 길이 API는 byte 단위이며 문자·grapheme 수와 혼용하지 않는다. 정규화, 대소문자 언어 규칙, BiDi와 글꼴 shaping은 별도 계층이다.

`Text`는 String의 별명이 아니라 지역화 ID·원문·형식 인자·카탈로그 revision의 수명을 갖는 불변 표시 문구 계층으로 구현하였다. `Name`은 pool domain을 구별하는 별도 intern 식별자다. 기능·의존성·검증 기준은 [문자열과 지역화 경계](StringAndText.ko.md)를 따른다.

## 5. 진단과 검증

SystemAllocator와 Arena·Stack·Pool·Binned·Budget·Debug·External allocator는 사용 범위별 live allocation·byte 계수 또는 통계를 제공한다. 상세 사용법과 확인되지 않은 외부 공급자 지원은 [할당기 설계와 사용 규약](Allocators.ko.md)에 정리한다. ObjectRegistry의 Stats는 상태와 보유 계수를, CaptureDiagnostics는 호출자가 준 버퍼에 객체별 스냅샷을 제공한다. 캡처는 할당하지 않으며 용량 부족 시 부분 기록하지 않는다. LastGcReport는 마지막 성공 GC의 검사·표식·retire·회수 개수와 ns 시간을 보존한다.

LogBuffer와 ProfileScope는 초기 고정 용량 진단이며 손실·잘림을 표시한다. 별도로 Logger가 여러 RuntimeLogSink에 기록을 전달한다. sink별 수준·카테고리 필터와 등록·제거, 동기 동시 접근과 오류 전파를 제공하며 TextLogSink와 stderr 출력이 첫 구현이다. Logger는 sink를 소유하지 않고, sink callback은 Logger에 재진입하지 않는다. 느리거나 중단된 동기 sink는 해당 Logger의 다른 기록·등록·제거도 지연시킨다. 파일 출력·회전·비동기 큐·포맷 API·전역 로거·전문 profiler viewer·crash 수집은 아직 구현되지 않았다. stderr 출력의 Windows 콘솔 표시와 POSIX 닫힌 pipe의 SIGPIPE 동작은 프로세스 환경·정책에 따른다.

Commandlet은 CPU-only 독립 실행 진입점과 명령 등록·목록·도움말을 제공한다. 현재 core-check 외의 에셋·cook·패키징·엔진 부트 명령은 구현되지 않았다. 상세 실행법과 인수·수명 계약은 Programs/Commandlet/README.ko.md를 따른다.

빌드 도구의 프로세스 정리는 제한 시간과 정리 작업 수 상한을 두지만, 운영체제 수준의 모든 분리 자손 격리나 실제 회수를 보장하지 않는다. stdout/stderr 수집 크기 제한은 아직 없으므로 과도한 출력이 메모리를 소모할 수 있다. 정리 완료를 확인하지 못한 빌드 임시 경로는 삭제하지 않고 실패로 보고한다.

Windows 재현 명령은 저장소 루트에서 실행한다. 사전에 .NET 10 SDK, Clang C++ 도구 체인과 `clang-format` 21.1.6이 필요하다. 컴파일러 자동 발견이 불가능하면 각 명령에 `/p:Compiler=<clang-cl 실행 파일 경로>`를 추가한다.

```text
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Debug /p:RocksTarget=Core
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Debug /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Release /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Asan /p:RocksTarget=Foundation
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll format --root .
out/dotnet-windows-foundation-debug/bin/FoundationTests.exe --list
out/dotnet-windows-foundation-debug/bin/FoundationTests.exe --filter Object --seed 7351 --repeat 100
```

macOS native host에서도 동일한 `dotnet msbuild` 타깃을 사용한다. OS별 빌드 정의와 실제 실행 성공은 구분한다. 별도 네이티브 빌드 및 설치 SDK·게임 통합 하네스의 실행법은 [빌드 안내](../../Build/README.md)를 따른다.

새 빌드 경로는 구성별 `out/dotnet-<플랫폼>-<타깃>-<구성>/`에 `foundation-report.json`, `build-evidence.json`, `action-report.json`과 도구 검사 보고서를 기록한다. 증거에는 소스 digest 전후, Git HEAD/dirty, compiler, seed, 단계 결과와 보고서 유효성을 기록한다. dirty 작업 트리의 검증을 clean commit 검증으로 표시하지 않는다.

### 이전 단계 실행 기록의 범위

다음 하위 절의 CTest·CMake/Python 결과와 테스트 수치는 각각 명시된 이전 소스와 해당 시점의 도구 경로에 한정된다. 현재 .NET 경로 또는 현재 변경분의 최종 Windows/macOS 결과가 아니다.

### 이전 String/Object 단계의 실행 기록

문자열 회귀 13개와 주석·LF 정리를 포함한 동일 소스로 다음 native 실행을 확인하였다. 로컬 compiler는 Clang 21.1.6이며 seed는 7351이다.

| 구성 | Foundation 테스트 | CTest 묶음 | 결과 |
|---|---:|---:|---|
| Windows Core-only Debug | 38개 | 5개 | PASS |
| Windows 전체 Debug | 65개 | 5개 | PASS |
| Windows 전체 Release | 65개 | 5개 | PASS |
| Windows 전체 AddressSanitizer | 65개 | 6개 | PASS — 실제 sanitizer UAF 음성 대조 포함 |

각 구성의 build-evidence는 sourceStableDuringRun=true, runnerReportCurrent=true를 기록한다. 로컬 검증 당시 소스 입력 80개의 digest는 `62cfbadfab18902669f767aae452a0a7a901be8eea67554cd14a02aa55b7b3c4`다. 로컬 검증 시점은 최초 설계 커밋 위의 dirty 작업 트리이며 clean commit 실행으로 표시하지 않는다.

같은 소스의 Release 전체 100회 반복, **65 × 100 = 6,500회 실행**도 실패 0으로 통과하였다. `out/windows-clang-release/repeat-report.json`과 `repeat-evidence.json`에 별도 보관하여 기본 실행 보고서를 덮어쓰지 않았다. 이는 해당 fixture 반복이며 전체 엔진의 장시간 실사용·플랫폼 통합 시험을 대체하지 않는다.

이후 구현을 `927f37f`에 커밋하고, CI의 macOS runner label만 지원되는 macOS 15로 바꾼 `ea0537c`에서 [Windows/macOS native CI](https://github.com/maxidea1024/rocks/actions/runs/42766665330)가 모두 통과하였다. 결과 artifact의 JSON·CTest 로그·build-evidence를 내려받아 직접 확인하였다.

| CI 구성 | 실제 호스트·컴파일러 | Foundation 테스트 | CTest | 결과 |
|---|---|---:|---:|---|
| Windows Debug | x64 · Clang 20.1.8 | 65개 | 5개 | PASS |
| macOS 15 Debug | ARM64 · Apple Clang 17.0.0 | 65개 | 5개 | PASS |
| macOS 15 ASan | ARM64 · Apple Clang 17.0.0 | 65개 | 6개 | PASS |

세 구성 모두 정확한 HEAD `ea0537cd6b9ac0947300eabc92eb889e05b83bde`, Git dirty=false, sourceStableDuringRun=true, runnerReportCurrent=true이며, 소스 입력 80개의 digest는 `11c67a2cf4e1724942fd0895f2f9a99be73c9d5cef2e5cb576bbb13498e1ecbb`로 일치한다. 앞선 로컬 digest와의 차이는 runner label 변경이다. 실제 Windows/macOS native 검증 완료는 이 Foundation subset에 한하며 Mac 에디터·입력·렌더러나 Intel Mac 실행까지 검증한 것은 아니다.

최종 문서 대조에서 보고서 쓰기 실패 시험의 범위가 과장된 것을 발견하여, 파일 생성 실패를 검증하는 자식 프로세스 회귀를 추가하였다. `556e5a28c9127d4929ae782350ae8a9dce112ec2`의 [최종 CI 실행 42767199402](https://github.com/maxidea1024/rocks/actions/runs/42767199402)에서 Windows Debug와 macOS Debug/ASan이 다시 각각 65개 테스트, CTest 5/5/6개를 통과하였다. 세 결과 모두 dirty=false·stable/currentReport=true이며 최종 소스 digest는 `8843d7f11f521d648b2b442a36c980a37a1117f52f3564f2a835f3608bb29a83`다. 이 후속 변경은 Python 회귀와 문서이며 엔진 C++ 코드는 바꾸지 않았다.


### Name·Text·플랫폼 확장: 당시 로컬 검증

이전 65개 단계에 Name 10개, Localization 24개, Platform 추가 11개를 더한 해당 단계 전체는 **110개**다. 플랫폼 API 이관과 경고 규약까지 포함한 동일 소스로 다음 실행을 확인하였다. compiler는 Clang 21.1.6, seed는 7351이다.

| 구성 | Foundation fixture | CTest | 결과 |
|---|---:|---:|---|
| Windows Core-only Debug | 83개 | 8개 | PASS |
| Windows 전체 Debug | 110개 | 8개 | PASS |
| Windows 전체 Release | 110개 | 8개 | PASS |
| Windows 전체 ASan | 110개 | 9개 | PASS |

모든 evidence가 sourceStableDuringRun=true, runnerReportCurrent=true, testsStatus=PASS다. 입력 116개 digest는 `e4b9e381004fda7b8c5252c241783496fe5a14a000a8d6d631ab7e27f57d02ed`다. Git HEAD `fce27b519e944db4229adb68f4fb46a7b5f031bc` 위 dirty 작업 트리 실행이며 clean commit 검증으로 표시하지 않는다.

CTest에는 실제 컴파일 경고의 오류 처리, 경고 옵션·pragma 강등 음성 대조, 공용 코드의 OS 의존 유입 감사·음성 대조가 포함된다. 소유 Thread의 Join 누락과 활성 NameLease를 남긴 pool 파괴도 자식 프로세스에서 Fatal을 확인한다. ASan 구성은 실제 UAF 대조군이 sanitizer 진단으로 실패해야 통과한다.

같은 Release 소스를 100회 반복하여 **110 × 100 = 11,000회** 모두 통과하였다. `out/windows-clang-release/repeat-report.json`과 `repeat-evidence.json`을 기본 보고서와 별도로 보관하였다. 종료 시 임시 소유권·allocator ledger 검사도 각 fixture의 규약대로 반복한다. 이 결과를 장시간 제품 워크로드나 모든 스케줄의 검증으로 확대하지 않는다.

교차 검토는 Name·Text 구현/테스트 분리, 플랫폼 수명·동시 제어, 빌드 정책의 독립 재검토로 진행하였다. 중복 Text 인자의 오류 코드 불일치, Windows SDK 매크로와 Yield 이름 충돌, 경고 감사의 플래그 누락·강등 누락을 수정하고 양의 work/depth 경계·catalog 정리 재진입·동시 Join/Wait 시험을 보강하였다. 상세 기록은 `ImplementationRound4.*.ko.md`를 따른다.

네이티브 Thread 생성/Join 실패 강제 주입, TSan, 임의 스케줄 전체 탐색, Linux/콘솔 backend는 미완료다. Null renderer·에디터·PIE의 실제 실행을 이 CPU 헤드리스 fixture 통과와 합산하지 않는다.


### Name·Text·플랫폼 확장: 최종 Windows/macOS CI

검증한 소스를 `b38c9437b32dcd41837b5df7b8ab2a1dbc622272`에 커밋한 뒤 [CI 실행 42771207691](https://github.com/maxidea1024/rocks/actions/runs/42771207691)에서 Windows와 macOS job이 모두 성공하였다. 내려받은 원본 JSON·CTest 로그·build-evidence를 직접 대조하였다.

| CI 구성 | 실제 호스트·컴파일러 | Foundation | CTest | 결과 |
|---|---|---:|---:|---|
| Windows Debug | x64 · Clang 20.1.8 | 110개 | 8개 | PASS |
| macOS 15 Debug | ARM64 · Apple Clang 17.0.0 | 110개 | 8개 | PASS |
| macOS 15 ASan | ARM64 · Apple Clang 17.0.0 | 110개 | 9개 | PASS |

세 구성 모두 위 HEAD, dirty=false, sourceStableDuringRun=true, runnerReportCurrent=true다. 보고서 해시도 실제 파일과 일치한다. 입력 116개의 digest `e4b9e381004fda7b8c5252c241783496fe5a14a000a8d6d631ab7e27f57d02ed`는 로컬 최종 빌드·11,000회 반복과 동일하다. warning_policy의 실제 경고 오류 대조군과 공용 OS 경계 감사가 각 호스트에서 실행된 것을 로그로 확인하였다. artifact는 `out/ci-42771207691`에 별도로 보관한다.

이는 CPU Foundation의 native 실행 검증이다. macOS Intel, Linux·콘솔, 전체 worker scheduler·병렬 GC, 에디터·렌더러·UI·PIE 통합은 별도 후속 Gate다. 리뷰 보고서의 ‘당시 미실행’ 문구는 검토 시점의 범위이고 이번 CI 실행 결과와 구분한다.

### GC·Reflection·Serialization 확장: 최종 로컬 검증

Clang 21.1.6, seed 7351로 다음 구성을 실제 실행하였다. 기존 110개에 worker·GC·Reflection·직렬화·metadata 접근 69개를 추가한 전체 179개다.

| 구성 | C++ fixture | CTest | 결과 |
|---|---:|---:|---|
| Windows Core-only Debug | 118개 | 8개 | PASS |
| Windows 전체 Debug | 179개 | 10개 | PASS |
| Windows 전체 Release | 179개 | 10개 | PASS |
| Windows 전체 ASan | 179개 | 11개 | PASS |

네 구성 모두 sourceStableDuringRun=true, runnerReportCurrent=true, testsStatus=PASS이며 실제 report SHA-256과 evidence가 일치한다. 소스 입력 147개의 digest는 `a321125b22a4ca628f5bbc15b3bb0f74b9ab1bfda003e46bb33ffa015e75b27a`다. HEAD `b38c9437b32dcd41837b5df7b8ab2a1dbc622272` 위 dirty 작업 트리 시험이며, clean commit 실행과 구분한다.

같은 소스로 Release 전체를 100회 반복하여 **179 × 100 = 17,900회** 실패 0을 확인하였다. repeat-report와 repeat-evidence를 기본 빌드 보고서와 분리하였다. 변형·N-fail 내부 반복을 이 숫자에 추가하지 않는다. 실제 Clang 메타 생성과 15개 음성 검사 그룹, 생성 헤더 단독 컴파일, 경고 오류·STL·플랫폼 경계 검사, ASan UAF 음성 대조가 포함된다.

독립 회귀에는 중첩 scheduler Submit의 바깥 Shutdown 교착 방지, 부분 marker 제출 OOM 후 drain, generated metadata 기반 cycle의 저장·복원 및 scratch 파괴 후 접근, native 참조 불일치의 생성 전 거부, 같은 native 타입의 별명 TypeId 보존이 포함된다. [5차 검증 기록](../Reviews/ImplementationRound5.FoundationValidation.ko.md)에 수정 근거와 범위 제한을 기록하였다.

### GC·Reflection·Serialization 확장: 최종 Windows/macOS CI

구현 커밋 `a310ba5dcd5f19eff4e8bf43a3d35d468a067e20`의 [CI 실행 42775786556](https://github.com/maxidea1024/rocks/actions/runs/42775786556)에서 Windows 및 macOS job이 모두 성공하였다. 원본 artifact의 report, build-evidence, CTest 로그를 내려받아 실행 목록·결과·해시를 직접 대조하였다.

| CI 구성 | 실제 compiler·architecture | Foundation | CTest | 결과 |
|---|---|---:|---:|---|
| Windows Debug | clang version 20.1.8 · x64 | 179개 | 10개 | PASS |
| macOS 15 Debug | Apple clang version 17.0.0 · arm64 | 179개 | 10개 | PASS |
| macOS 15 ASan | Apple clang version 17.0.0 · arm64 | 179개 | 11개 | PASS |


세 구성 모두 위 HEAD, dirty=false, sourceStableDuringRun=true, runnerReportCurrent=true다. source digest `a321125b22a4ca628f5bbc15b3bb0f74b9ab1bfda003e46bb33ffa015e75b27a`와 147개 입력 파일은 로컬 네 구성 및 17,900회 반복과 동일하다. 실제 report SHA-256도 각각 evidence와 일치한다. 새 독립 회귀를 포함한 179개 이름의 집합, seed 7351, 메타 컴파일러 15개 그룹, 경고 오류·플랫폼/STL 감사의 실행 성공을 확인하였다. macOS ASan은 실제 UAF 음성 대조를 포함한다.

artifact는 `out/ci-42775786556`에 보관한다. 별도의 검토자가 로컬 결과와 CI 원본 산출물을 독립 대조하여 같은 커밋·소스·실행 목록·보고서 해시 및 실제 음성 대조군 성공을 확인하였다. 불일치는 없었다. 문서만 갱신한 후속 커밋은 이 C++/빌드 입력 digest를 변경하지 않는다. 이는 CPU Foundation subset의 native 실행 결과이며 전체 엔진·Intel Mac·콘솔·TSan 검증으로 확대하지 않는다.

### 테스트 범위

- 할당기 정렬·실패·소유권·스레드 계수, Array 성장·자기 alias·소멸 수, Span stride 검증.
- Function/ModuleLease의 정리 callback 재진입, capture 반환 뒤 task 완료, logger 잘림·포화.
- 스키마 원본 변경 후 독립 복사본 읽기·deep copy·실패 복구·활성 reader 및 코드 module lease.
- registry/context/type/generation 분리, 비이동 주소, generation 고갈 격리.
- 생성 비공개성·역순 rollback·일괄 publication·OOM 복구.
- 별도 인접 배열/DFS 기준 그래프와 GC 결과 비교, 2,048-node 깊은 그래프, scratch OOM에서 잘못된 sweep 금지.
- logical destroy, root/강한 간선 보존, weak pin 거부, borrow Busy, backend 지연·종료 재시도, 소멸자 재진입.
- 잘못된 CLI·불일치 filter·보고서 파일 생성 실패·assert 종료·잠긴 stderr·worker 실패 집계를 실제 자식 프로세스로 검사. 보고서 출력 중 디스크 용량 소진·close 실패의 전용 주입 시험은 아직 실행하지 않았다.
- ASan preset에서는 의도적인 해제 후 접근이 실제 sanitizer 진단과 함께 실패하는 음성 대조군을 추가한다.

Windows ASan은 LeakSanitizer가 아니며, 소유권 ledger와 종료 검사를 대체하지 않는다. 모든 실행 환경에서 메모리 누수가 절대 없다는 주장은 하지 않는다. 알려진 raw pointer 주소 재사용 한계, 수동 Trace의 완전성, wrapper보다 오래 살아야 하는 Registry 계약을 명시하고 회귀 테스트를 확대한다.

## 6. 교차 검토에서 수정한 항목

- Span의 derived 배열→base 배열 변환으로 발생할 수 있는 잘못된 stride를 차단하였다.
- Object/ObjectRef 복사·이동으로 정체성과 간선 변경 검사를 우회하지 못하게 하였다.
- DestroyRequested 객체의 기존 RootRef 복제가 저장소 보존 규약을 지키도록 수정하였다.
- Function/ModuleLease의 이전 capture 정리 중 재진입 대입으로 새 소유권을 잃던 경로를 수정하였다.
- assertion 경로의 CRT stderr lock 의존, 테스트 실패 집계의 worker 경합, profile 이름 잘림 정보 유실을 수정하였다.
- 주석 검토에서는 Shutdown 닫기 전 실패, callback 재진입 예외, 타입 식별자 표현, wrapper 동시 사용 제한을 보완하였다.

근거는 `Docs/Reviews/ImplementationRound*.ko.md`와 각 회귀 테스트에 둔다. 과거 회차 보고서는 당시 실행 범위를 보존하며 5차 Foundation 확장 검증 기록 역시 해당 소스의 결과로 구분한다.

## 7. 이번 범위와 이후 작업

현재 범위는 Core·Object의 실행 경로와 C#/.NET 기반 빌드·메타 도구의 초기 구현이다. 각 경로의 구현과 검증 결과를 전체 Core 또는 엔진 기능의 완료로 확대하지 않는다.

전체 Foundation에는 frame 수명 관리·자동 일괄 해제 stack·thread cache 등 후속 할당기, 가상 메모리 오류 주입 검증, queue 및 기타 후속 컨테이너, Path·파일 IO, atomics/RWLock, root owner 진단, 정밀한 빌드 의존 추적과 메타 unit 자동 발견, 장시간 스트레스·대규모 pause 성능 검증 등이 남아 있다. 수학·기본 충돌 라이브러리, 엔진 틱, Component와 상위 에디터·PIE·입력·렌더러 통합도 별도 후속 작업이다. 전체 C00–C26/Foundation Gate는 미완료이며 초기 선형 탐색과 고정 상한을 제품 성능 충족으로 간주하지 않는다.
