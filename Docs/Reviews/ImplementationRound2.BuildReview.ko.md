# 구현 2차 교차 검토 — Core·Platform·빌드

검토일: 2026-11-10. 검토 대상은 `C:\work\rocks`의 Array, Span, Result, SystemAllocator, Win/Mac memory·clock·mutex 구현과 관련 시험이다. 담당 Core 구현 파일은 직접 수정하지 않았다.

## 실제 확인한 빌드 결과

Windows x64, clang-cl 21.1.6, Visual Studio 2019 C++ tools/Windows SDK, Ninja로 `windows-clang-core`를 실제 configure/build/test하였다. `ROCKS_BUILD_OBJECT=OFF`에서 CoreFoundationTests를 생성하므로 Object 구현의 완료를 기다리지 않는다.

- Core·Platform·C06 테스트 22개 PASS(seed 7351, 각 1회).
- CTest 4개 PASS: foundation, harness_negative_controls, runtime_stl_audit, scaffold_transaction.
- 각 공개 헤더를 단독 첫 include로 둔 translation unit을 컴파일하였다.
- 예외·RTTI를 끈 `/W4 /WX` 구성을 사용하였다.
- macOS 네이티브 컴파일·실행 및 sanitizer는 미실행이다. CI workflow 파일의 존재를 실행 증거로 계산하지 않는다.

위 시험 결과만으로 아래에서 발견한 API 타입 안전성 결함을 배제할 수 없었다.

## IR2-B01 · P1 — Span의 Derived→Base 변환에 따른 잘못된 배열 간격

**수정 전 위치:** `Engine/Core/Public/Core/Containers/Span.h:23–25`의 `requires(__is_convertible_to(U*, T*))`, 같은 파일의 `Span(T* data, usize count)` 생성자.

`Derived*`는 `Base*`로 변환할 수 있지만 `Derived[]`의 인접 원소 간격은 일반적으로 `Base[]`와 다르다. 기존 조건은 `Span<Derived>`를 `Span<Base>`로 변환하도록 허용한다. 직접 `Span<Base>(derivedArray, count)`를 호출하는 경로도 같은 문제를 가진다.

예를 들어 `Base { int value; }`, `Derived : Base { int extra; }`와 두 개의 Derived 원소가 있으면 변환된 `Span<Base>[1]`은 두 번째 Derived의 Base가 아니라 첫 번째 원소의 extra 위치를 참조할 수 있다. 공개 view 계약이 허용한 생성 방식만으로 잘못된 값을 읽거나 다른 필드를 덮어쓸 수 있으므로 단순한 진단 개선이 아니다.

**최소 수정:** 두 생성 경로 모두 배열 원소 호환성 `U (*)[] → T (*)[]`로 제한한다. 같은 원소 타입에 const/volatile을 추가하는 변환만 허용하고 Derived→Base 변환과 const 제거를 거부한다. `nullptr, 0`은 별도 명시 생성 경로로 유지할 수 있다.

**수용 기준:** `Span<Derived> → Span<Base>`와 `Derived* + count → Span<Base>`가 모두 compile-time constructibility 검사에서 false이고, `Span<T> → Span<const T>` 및 T 배열의 정상 view는 true다. null+0, 빈 view, mutable→const 조회 회귀와 실제 Core 재빌드가 통과해야 한다.

Core 담당자가 두 경로의 결함을 확인하고 수정 및 compile-time negative regression을 진행 중이다. 최종 closure는 수정 후 빌드 결과로 아래에 추가한다.

## 나머지 구현의 검토 결과와 제한

| 영역 | 확인 내용 | 판정 |
|---|---|---|
| Array 실패 원자성 | 필요한 새 저장소를 먼저 할당하고, 실패 전 기존 원소를 move하지 않는다. 자기 원소를 append하는 경우 기존 저장소가 살아 있을 때 새 원소를 먼저 구성한다. 이후 이동·소멸을 noexcept 타입에 한정한다. | 새 확정 결함 없음. 별도 N-fail·self-alias·move-only·cross-allocator move·정렬 시험이 현재 실행됐다. |
| Array 크기·정렬 | capacity×sizeof(T)와 성장 상한을 확인하고 alignof(T)로 할당한다. Clear는 원소만 역순 파괴하며 저장소는 유지한다. | 현재 시험에서 overflow 시 상태 보존과 64-byte 정렬이 확인됐다. 모든 사용자 정의 타입의 재배치 의미를 보장한다는 뜻은 아니다. |
| Result 수명 | success payload와 failure 상태를 분리하고 move-only 타입을 지원한다. move된 원본의 유효한 moved-from 값도 나중에 파괴한다. Failure(None)은 InvalidArgument로 바꾼다. | 현재 move/재대입/파괴 ledger 시험 PASS. 실패 상태의 Value 호출을 차단하는 fatal 경로의 subprocess 시험은 아직 없다. |
| SystemAllocator | ledger node와 payload를 확보한 뒤에만 mutex 아래에서 등록한다. payload allocation 실패 시 ledger 저장소를 반환한다. unknown pointer는 역참조하지 않고 ledger의 주소 비교로만 찾는다. | 본문상 rollback·소유권·통계 일관성을 확인했다. payload/ledger 두 내부 OS allocation의 독립 실패를 강제로 주입하는 시험은 아직 없다. |
| cross-thread free | 조회·등록·해제를 하나의 mutex로 보호하고, 제거한 record를 lock 밖에서 반환한다. 테스트는 allocation workers를 join한 뒤 다른 workers가 blocks를 반환한다. | 현재 실제 Windows 시험 PASS. allocator 자체는 모든 사용자와 호출이 종료된 뒤 파괴해야 한다. |
| memory/clock/mutex | alignment 0/비거듭제곱/overflow 검사를 수행하고 작은 정렬은 pointer 정렬까지 높인다. monotonic clock과 OS thread ID를 제공한다. mutex는 명시적으로 nonrecursive다. | Win 시험 PASS. macOS는 코드 검토만 수행했고 native 실행은 NOT RUN이다. clock의 nanosecond 단위 반환이 1ns 실제 해상도를 의미하지 않는다. |
| STL 경계 | 엔진 소유 소스는 자체 타입/컨테이너를 사용한다. placement new의 `<new>`와 C/OS API는 사용하지만 STL 컨테이너·알고리즘·`std::` 사용은 없다. 시험 runner의 stdio/stdlib/string/errno는 C API다. | lexical audit PASS. OS/CRT 헤더 내부나 바이너리의 전이 의존성 전체를 검증한 것으로 표현하지 않는다. |

## 검증 보강 후보 — 현재 결함과 구분

1. `Result::Value` on failure, Span/Array 범위 위반, live allocation이 남은 allocator 종료를 subprocess에서 실행하여 오류 종료와 진단을 확인한다. 현재 harness의 CHECK/REQUIRE 음성 대조는 해당 엔진 fatal 경로의 대체물이 아니다.
2. SystemAllocator의 첫 ledger allocation 실패와 두 번째 payload allocation 실패를 독립적으로 주입하여 stats 변화 0·OS 저장소 잔존 0을 확인한다. 현재 NFailAllocator는 SystemAllocator 바깥 호출을 실패시키므로 내부 두 단계의 rollback을 직접 실행하지 않는다.
3. `Deallocate(void*)`의 repeated-free 검출은 그 주소가 다시 할당되지 않은 상태의 ledger membership 검사다. 이미 해제한 원시 주소가 새 allocation에 재사용된 뒤까지 generation 안전성을 제공한다고 문서화하면 안 된다. 그런 보장이 필요한 상위 API에는 별도 handle/generation 계약이 필요하다.

이 후보들은 실행되지 않은 검증 범위를 명시한 것이다. 코드에서 확인한 IR2-B01과 같은 재현 가능한 타입 안전성 결함으로 합산하지 않는다.

## 담당 빌드 변경

Core-only preset/target을 추가하여 초기 구현을 독립 빌드할 수 있게 하였다. `.gitignore`의 `/build/` 항목을 제거하여 소스 `Build/`의 추적을 방해하지 않도록 하였다. 기존 `/out/`, `/Build/Artifacts/`, `/Build/Cache/` 제외는 유지한다. 전체 Object 통합 빌드는 해당 구현이 준비된 뒤 별도 결과로 보고한다.


## 수정 후 closure — IR2-B01

Core 담당자가 Span의 pointer/count 및 Span 변환 생성자 모두에 `U (*)[] → T (*)[]` 호환성 조건을 적용하였다. `Span<Base>`의 두 Derived 생성 경로를 거부하는 compile-time negative assertion, mutable→const 허용, null+0 조회가 추가되었다. 최신 전체 Debug에서 해당 헤더 및 회귀 시험이 실제 컴파일·실행되어 **IR2-B01 해결 확인**으로 판정한다.

2026-11-10 현재 Windows 전체 Debug 51개 및 뒤에 추가된 metadata lease 회귀를 포함한 ASan 52개 시험이 통과하였다. 최신 최종 구성별 시험 횟수·소스 digest·Git dirty 상태는 각 `out/<preset>/build-evidence.json`과 `foundation-report.json`을 정본으로 사용한다. 이 문서의 최초 22개 Core 시험 수는 검토 시작 시점의 결과다.

다른 담당자의 C00/C06 교차 검토 IC01–IC04는 본인이 소유한 Function/ModuleLease 재진입 대입, AssertFailed의 CRT lock 의존, 하네스의 병렬 실패 집계, ProfileScope truncation 표시를 수정하고 실제 회귀를 추가하였다. 독립 재검토자는 `ImplementationRound3.CoreClosure.ko.md`에서 해결을 확인하였다.

## 최종 LF 확정 소스의 실행 결과

주석·String/StringView·LF 정규화까지 완료한 동일 소스로 네 native Windows 구성을 다시 configure/build/test하였다. 초기 시점의 수치는 위 기록에 보존하고, 최종 결과는 다음과 같이 확정한다.

| 구성 | C++ 시험 | CTest | 결과 |
|---|---:|---:|---|
| windows-clang-core | 38 | 5 | PASS |
| windows-clang-debug | 65 | 5 | PASS |
| windows-clang-release | 65 | 5 | PASS |
| windows-clang-asan | 65 | 6 | PASS |

ASan의 여섯 번째 검사는 의도적인 heap-use-after-free를 별도 자식 프로세스에서 발생시켜 실제 AddressSanitizer 진단과 실패 종료를 확인한다. 정상 시험의 무오류 결과와 이 음성 대조군의 성공을 구분한다. Windows ASan은 별도 LeakSanitizer의 실행 증거가 아니며 엔진의 소유권·allocation ledger 검사를 함께 사용한다.

Release의 전체 65개 시험을 seed 7351로 각각 100회 실행하여 6,500회 호출에서 실패 0건을 확인하였다. 기존 foundation-report.json을 덮지 않고 `out/windows-clang-release/repeat-report.json`, `repeat-output.log`, `repeat-evidence.json`에 별도로 기록하였다.

네 구성의 `build-evidence.json`과 반복 실행의 증거가 가리키는 source digest는 모두 `62cfbadfab18902669f767aae452a0a7a901be8eea67554cd14a02aa55b7b3c4`이며 source fileCount는 80이다. 각 실행 구간의 sourceStableDuringRun, runnerReportCurrent는 true다. Git 상태는 실제 dirty 상태로 기록하였으며, 아직 커밋하지 않은 소스를 clean commit으로 표시하지 않았다. 최종 clang-format 21.1.6 검사도 통과하였다. macOS 실제 CI 결과는 이 Windows 검증 기록에 포함하지 않는다.
