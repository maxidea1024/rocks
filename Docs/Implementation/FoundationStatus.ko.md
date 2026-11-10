# Rocks Foundation 최초 구현 상태

기준일: 2026-11-10. 이 문서는 **Core·Object의 첫 실행 가능한 구현 범위**를 기록한다. C00–C13 전체 완료, Foundation Gate 통과, 제품용 엔진 완성을 뜻하지 않는다. 설계 PDF와 작업 CSV는 목표 기준이며 실제 진척은 이 문서와 실행 증거를 함께 확인한다.

## 1. 현재 동작하는 경로

`Base → PlatformBase → Core → MetaSchema/Task → ObjectRuntime → CPU 전용 테스트 호스트`를 빌드한다. 렌더러·윈도·웹 에디터 없이 생성, 참조, GC, 종료와 실패 주입을 실행할 수 있다. 현재 Task는 동기 실행기이며 GC는 객체를 이동하지 않는 단일 소유 스레드 full STW 방식이다.

엔진 소유 Runtime과 공개 API에는 STL을 사용하지 않는다. C++23, 예외 비활성, RTTI 비활성, 공개 헤더 단독 컴파일, 경고를 오류로 처리하는 설정을 사용한다. OS/컴파일러 기반 기능은 Platform과 compiler intrinsic 경계로 제한한다.

## 2. 모듈·파일과 구현 범위

| 작업 | 현재 구현 | 아직 구현하지 않은 범위 |
|---|---|---|
| C00 | CMake 분리 target, Windows/macOS preset, 빌드 진입점, 서식·STL 검사, 단독 헤더 검사, 테스트 호스트, 모듈 템플릿 plan/apply/validate, CI 정의 | 전체 프로젝트·컴포넌트·프리팹 템플릿, 복구 저널, RocksBuild, 캐시·분산 빌드 |
| C01 | Base 정수 타입, Guid POD, checked arithmetic | 전체 ABI·compiler 추상화 |
| C02 | 정렬 할당, 단조 시계, 스레드 식별, mutex, 최소 fatal sink | 가상 메모리 page, 일반 thread/atomic 서비스 |
| C03 | Result/Error, Move/Forward, 제한된 traits | 전체 범용 utility 집합 |
| C04 | SystemAllocator 소유권 ledger·통계, 실패 주입 테스트 | page/arena/pool, 주소 검색 최적화, OS 내부 실패 전용 hook |
| C05 | Array, Span, 실패 시 상태 유지, 비소유 뷰 규약 | hash map, set, queue 등 후속 컨테이너 |
| C06 | Function, ModuleLease, 동기 InlineExecutor, 고정 용량 로그·프로파일 scope, assert, 테스트·음성 대조군 | worker/dependency/cancel/lane scheduler, 비동기 로그 writer·viewer |
| C07 | 수동 TypeSchema, 깊은 복사, 불변 revision lease, 코드 모듈 lease | Clang 생성 메타데이터, 일반 IsA, Archive |
| C08 | 비이동 64-slot chunk, registry domain·generation, 고갈 슬롯 격리, 진단 캡처 | 빠른 인덱스 조회, 대규모 벤치마크 |
| C09 | RootRef/WeakRef/ObjectRef/Borrow/BackendLease, 문맥·스레드 검사 | root owner chain, 전체 safepoint 시스템 |
| C10 | 비공개 생성 트랜잭션, 간선 검증 후 일괄 공개, 역순 롤백 | ClassDefaults·instancing, 트랜잭션 활성 중 GC, 전체 lifecycle hook |
| C11 | 반복형 full STW mark/sweep, scratch OOM 시 sweep 금지, 독립 그래프 oracle | 점진·병렬 GC, 시간 예산·write barrier |
| C12 | logical destroy·retire·실제 회수 분리, Busy 종료·재시도, 모의 backend lease | 실제 GPU/IO/audio 연동, 장시간 시스템 스트레스 |
| C13 subset | 자체 UTF-8 String/StringView, 명시 allocator·move/clone·UTF 검증·alias/OOM 회귀 13개 | Name intern, Guid 생성/parse, hash 정책, 플랫폼 변환, 지역화 Text |

주요 파일 위치는 다음과 같다.

- `Engine/Core/Public/Core/Memory`, `Containers`, `Text`: 메모리·컨테이너·문자열 계약.
- `Engine/Core/Public/Core/Function.h`, `Lifetime`, `Diagnostics`: callable·코드 수명·시스템 진단.
- `Engine/ObjectRuntime/Public/Object`: 타입 등록·핸들·참조·생성·GC 공개 API.
- `Engine/ObjectRuntime/Private`: 스키마 저장소, 비이동 Registry와 수집·회수 구현.
- `Tests/Core`, `Tests/Platform`, `Tests/Object`, `Tests/Foundation`, `Tests/Integration`: 단위·통합·장애·음성 대조군.
- `Build/build.py`, `Build/format.py`, `Build/audit_runtime.py`: 빌드와 검증 진입점.

## 3. 반드시 지켜야 할 수명·동시성 계약

1. Allocator → TypeRegistry → ObjectRegistry 순으로 생성하고 반대로 정리한다. 각 wrapper/뷰는 소유자가 살아 있는 동안만 사용할 수 있다. 종료 실패를 무시하고 먼저 파괴하면 강제 해제하지 않고 fatal로 드러낸다.
2. 객체 접근·생성·변경·GC는 Registry 생성 스레드 전용이다. gate는 상태와 계수를 보호하며 객체 필드까지 스레드 안전하게 만들지는 않는다. 서로 다른 RootRef의 Clone/Reset, 반환할 Borrow/BackendLease의 Reset은 정해진 범위에서 다른 스레드에도 허용한다. 동일 wrapper를 동시에 변경하지 않는다.
3. RootRef는 외부 GC 루트, ObjectRef는 소유 객체 안의 강한 간선, WeakRef는 비소유 값 핸들이다. Borrow는 제한된 포인터 접근권이며 하나라도 활성 상태면 Collect가 Busy를 반환한다. BackendLease는 해당 저장소 회수만 지연하며 루트나 자식 접근권은 아니다.
4. Object와 ObjectRef의 복사·이동은 금지한다. ObjectRef는 SetReference를 통해 변경하고 소유자의 Trace가 빠짐없이 방문해야 한다. 현재는 객체 안에 직접 포함된 필드만 지원한다. 사용자 Trace 누락과 임의 raw pointer 오용을 자동 증명하는 단계는 아니다.
5. RequestDestroy는 논리적 접근을 닫는다. 외부 RootRef에서 도달 가능한 동안 파괴 요청 객체와 그 강한 간선의 자식을 보존하며, 도달 불가능한 cycle은 수거한다. 기존 RootRef의 Clone과 새로운 WeakRef pin은 다르게 처리한다.
6. ConstructionContext는 전체 공개 전까지 객체를 격리한다. 실패 가능한 초기화는 호출자가 결과를 확인하고 Commit을 생략한다. 첫 staged 객체만 루트로 반환되므로 나머지는 그 객체에서 도달하거나 별도 루트가 있어야 한다.
7. GC는 작업 큐를 먼저 확보한다. OOM이나 잘못된 간선으로 mark가 끝나지 않으면 sweep하지 않는다. 사용자 생성자·Trace·소멸자와 할당기 콜백은 Registry 잠금 밖에서 실행한다. 변경 재진입은 거부하며 활성 생성 트랜잭션의 재진입 롤백은 금지한다.
8. Shutdown은 새 소유권 유입을 닫고 회수를 시도한다. 보유권이 남으면 Busy이며 반환 후 재시도한다. 접근 검증이나 processing 재진입 거부는 닫기 전에 반환한다. 닫은 뒤의 회수 실패는 닫힌 상태를 유지한다.

각 계약은 해당 공개 헤더와 복잡한 처리 단계에 한국어 주석으로도 남긴다. [코드 스타일](../CodingStyle.md)은 주석의 정확성과 갱신을 리뷰 기준으로 요구한다.

## 4. 문자열과 표시 문구

`std::string`이나 `std::wstring`을 도입하지 않는다. `String`은 Allocator 기반 UTF-8 소유 값, `StringView`는 길이 기반 비소유 뷰다. 길이 API는 byte 단위이며 문자·grapheme 수와 혼용하지 않는다. 정규화, 대소문자 언어 규칙, BiDi와 글꼴 shaping은 별도 계층이다.

`Text`는 String의 별명으로 만들지 않는다. 지역화 ID·원문·언어 변경·형식 인자·카탈로그 revision의 수명을 갖는 표시 문구 계층으로 계획한다. `Name`도 별도 intern 식별자다. 기능·의존성·검증 기준은 [문자열과 지역화 경계](StringAndText.ko.md)를 따른다.

## 5. 진단과 검증

SystemAllocator는 live allocation·byte 계수를 제공한다. ObjectRegistry의 Stats는 상태와 보유 계수를, CaptureDiagnostics는 호출자가 준 버퍼에 객체별 스냅샷을 제공한다. 캡처는 할당하지 않으며 용량 부족 시 부분 기록하지 않는다. LastGcReport는 마지막 성공 GC의 검사·표식·retire·회수 개수와 ns 시간을 보존한다.

LogBuffer와 ProfileScope는 초기 고정 용량 진단이며 손실·잘림을 표시한다. 전문 profiler viewer·백그라운드 writer·crash 수집을 완료했다고 간주하지 않는다.

Windows 재현 명령:

```text
python Build/build.py --preset windows-clang-core
python Build/build.py --preset windows-clang-debug
python Build/build.py --preset windows-clang-release
python Build/build.py --preset windows-clang-asan
python Build/format.py --check
out/windows-clang-debug/FoundationTests.exe --list
out/windows-clang-debug/FoundationTests.exe --filter Object --seed 7351 --repeat 100
```

macOS는 native host에서 `macos-clang-debug`, `macos-clang-asan` preset을 사용한다. preset과 CI 정의가 있는 것과 해당 OS에서 실제 성공한 것은 구분한다.

실행마다 `out/<preset>/foundation-report.json`, `build-evidence.json`, `Testing/Temporary/LastTest.log`를 남긴다. 빌드 증거에는 소스 digest 전후, Git HEAD/dirty, compiler, seed, 단계 결과와 report 유효성을 기록한다. dirty 작업 트리 시험을 clean commit 시험으로 표기하지 않는다. 최종 실행 수치는 아래 확정 기록과 별도 검토 보고서에서 확인한다.

### 최종 실행 기록

문자열 회귀 13개와 주석·LF 정리를 포함한 동일 소스로 다음 native 실행을 확인했다. 로컬 compiler는 Clang 21.1.6이며 seed는 7351이다.

| 구성 | Foundation 테스트 | CTest 묶음 | 결과 |
|---|---:|---:|---|
| Windows Core-only Debug | 38개 | 5개 | PASS |
| Windows 전체 Debug | 65개 | 5개 | PASS |
| Windows 전체 Release | 65개 | 5개 | PASS |
| Windows 전체 AddressSanitizer | 65개 | 6개 | PASS — 실제 sanitizer UAF 음성 대조 포함 |

각 구성의 build-evidence는 sourceStableDuringRun=true, runnerReportCurrent=true를 기록한다. 로컬 검증 당시 소스 입력 80개의 digest는 `62cfbadfab18902669f767aae452a0a7a901be8eea67554cd14a02aa55b7b3c4`다. 로컬 검증 시점은 최초 설계 커밋 위의 dirty 작업 트리이며 clean commit 실행으로 표시하지 않는다.

같은 소스의 Release 전체 100회 반복, **65 × 100 = 6,500회 실행**도 실패 0으로 통과했다. `out/windows-clang-release/repeat-report.json`과 `repeat-evidence.json`에 별도 보관하여 기본 실행 보고서를 덮어쓰지 않았다. 이는 해당 fixture 반복이며 전체 엔진의 장시간 실사용·플랫폼 통합 시험을 대체하지 않는다.

이후 구현을 `927f37f`에 커밋하고, CI의 macOS runner label만 지원되는 macOS 15로 바꾼 `ea0537c`에서 [Windows/macOS native CI](https://github.com/maxidea1024/rocks/actions/runs/42766665330)가 모두 통과했다. 결과 artifact의 JSON·CTest 로그·build-evidence를 내려받아 직접 확인했다.

| CI 구성 | 실제 호스트·컴파일러 | Foundation 테스트 | CTest | 결과 |
|---|---|---:|---:|---|
| Windows Debug | x64 · Clang 20.1.8 | 65개 | 5개 | PASS |
| macOS 15 Debug | ARM64 · Apple Clang 17.0.0 | 65개 | 5개 | PASS |
| macOS 15 ASan | ARM64 · Apple Clang 17.0.0 | 65개 | 6개 | PASS |

세 구성 모두 정확한 HEAD `ea0537cd6b9ac0947300eabc92eb889e05b83bde`, Git dirty=false, sourceStableDuringRun=true, runnerReportCurrent=true이며, 소스 입력 80개의 digest는 `11c67a2cf4e1724942fd0895f2f9a99be73c9d5cef2e5cb576bbb13498e1ecbb`로 일치한다. 앞선 로컬 digest와의 차이는 runner label 변경이다. 실제 Windows/macOS native 검증 완료는 이 Foundation subset에 한하며 Mac 에디터·입력·렌더러나 Intel Mac 실행까지 검증한 것은 아니다.

### 테스트 범위

- 할당기 정렬·실패·소유권·스레드 계수, Array 성장·자기 alias·소멸 수, Span stride 검증.
- Function/ModuleLease의 정리 콜백 재진입, capture 반환 뒤 task 완료, logger 잘림·포화.
- 스키마 원본 변경 후 독립 복사본 읽기·deep copy·실패 복구·활성 reader 및 코드 module lease.
- registry/context/type/generation 분리, 비이동 주소, generation 고갈 격리.
- 생성 비공개성·역순 rollback·일괄 publication·OOM 복구.
- 별도 인접 배열/DFS 기준 그래프와 GC 결과 비교, 2,048-node 깊은 그래프, scratch OOM에서 잘못된 sweep 금지.
- logical destroy, root/강한 간선 보존, weak pin 거부, borrow Busy, backend 지연·종료 재시도, 소멸자 재진입.
- 잘못된 CLI·불일치 filter·보고서 쓰기 실패·assert 종료·잠긴 stderr·worker 실패 집계를 실제 자식 프로세스로 검사.
- ASan preset에서는 의도적인 해제 후 접근이 실제 sanitizer 진단과 함께 실패하는 음성 대조군을 추가한다.

Windows ASan은 LeakSanitizer가 아니며, 소유권 ledger와 종료 검사를 대체하지 않는다. 모든 실행 환경에서 메모리 누수가 절대 없다는 주장은 하지 않는다. 알려진 raw pointer 주소 재사용 한계, 수동 Trace의 완전성, wrapper보다 오래 살아야 하는 Registry 계약을 명시하고 회귀 테스트를 확대한다.

## 6. 교차 검토에서 수정한 항목

- Span의 derived 배열→base 배열 변환으로 발생할 수 있는 잘못된 stride를 차단했다.
- Object/ObjectRef 복사·이동으로 정체성과 간선 변경 검사를 우회하지 못하게 했다.
- DestroyRequested 객체의 기존 RootRef 복제가 저장소 보존 규약을 지키도록 수정했다.
- Function/ModuleLease의 이전 capture 정리 중 재진입 대입으로 새 소유권을 잃던 경로를 수정했다.
- assertion 경로의 CRT stderr lock 의존, 테스트 실패 집계의 worker 경합, profile 이름 잘림 정보 유실을 수정했다.
- 주석 검토에서는 Shutdown 닫기 전 실패, 콜백 재진입 예외, 타입 식별자 표현, wrapper 동시 사용 제한을 보완했다.

근거는 `Docs/Reviews/ImplementationRound*.ko.md`와 각 회귀 테스트에 둔다. 과거 회차 보고서는 당시 실행 범위를 보존하며 최신 결과는 이 문서의 최종 기록으로 구분한다.

## 7. 다음 구현 순서

첫 범위의 빈칸을 닫으며 API/테스트 계약을 안정화한다. 메모리 서비스·기본 컨테이너·문자열/식별자·파일/직렬화·Task worker·Clang 메타 생성기를 의존 순서대로 확장한다. `Text`는 문자열 기반 위에서 localization catalog와 formatting adapter를 분리하고, 입력/IME·UI는 그 상위 소비자로 연결한다.

이번 Windows/macOS Foundation 검증을 기반으로 실제 모듈/비동기 작업 drain, 긴 수명 스트레스와 에디터·입력 통합을 확장한 뒤 점진·병렬 GC를 진행한다. 초기 계수·고정 버퍼와 선형 lookup이 제품용 성능 목표를 달성했다고 가정하지 않는다. 전체 C00–C26 및 Foundation Gate의 미실행 항목은 계속 미완료로 유지한다.
