# 구현 교차 검토 2차 — C00/C06 Core·Task·진단·하네스

검토일: 2026-11-10. 검토자는 C01–C05 작성자이며 이번에는 다른 작성자의 Function, ModuleLease, InlineExecutor, LogBuffer/ProfileScope, 공통 테스트 하네스와 빌드 정책을 검토하였다. 해당 production 소스는 수정하지 않았다. 아래 위치는 지적을 전달한 수정 전 소스 기준이다.

이 보고서는 정적 소스 검토와 구체적인 실행 순서 반례다. 이 보고서의 재현 fixture를 실제 실행하거나 sanitizer에서 확인한 결과로 표현하지 않는다. 별도 Windows 컴파일·실행 여부는 실제 빌드 로그를 따른다. macOS 실행 결과는 이 검토에 없다.

## 1. 판정

수정 필요 사항은 P1 2건, P2 2건이다. 두 P1은 root에 즉시 전달하였다. 첫 구현의 동기·단일 owner 범위와 아직 구현하지 않은 Task Graph/동적 DLL unload를 구분하여 검토하였다.

| ID | 등급 | 결함 |
|---|---|---|
| IC01 | P1 | Function·ModuleLease move assignment에서 old destructor/release 재진입 후 설치된 소유권을 덮어씀 |
| IC02 | P1 | invariant 실패 경로가 CRT stderr lock/stdio에 의존하여 교착 가능 |
| IC03 | P2 | native worker에서 CHECK를 호출하면 전역 failure counter에 data race |
| IC04 | P2 | ProfileScope가 이름을 먼저 자른 뒤 truncated=false로 기록 |

## 2. IC01 — 소멸·release 재진입 후 소유권 덮어쓰기

**위치:** `Engine/Core/Public/Core/Function.h:26–30`, `Function.h:67–77`; `Engine/Core/Public/Core/Lifetime/ModuleLease.h:49–59`, `ModuleLease.h:66–74`.

두 move assignment 모두 현재 상태를 Reset한 뒤 incoming 상태를 설치한다. Reset은 자기 필드를 비운 뒤 외부 destructor/release callback을 호출한다. 그 callback이 같은 target을 다시 변경할 수 있지만, 바깥 assignment는 callback 복귀 후 target의 새 상태를 검사하지 않고 덮어쓴다.

**Function 실행 순서:**

1. target은 callable A, incoming은 B, 별도 prepared는 C를 소유한다.
2. A의 noexcept destructor는 `target = Move(prepared)`를 실행하도록 구성한다. C는 미리 생성하므로 destructor 안의 추가 할당도 필요 없다.
3. `target = Move(incoming)`이 바깥 Reset을 시작한다. target 필드가 빈 상태가 된 뒤 A destructor가 C를 target에 설치한다.
4. A destructor가 반환하면 바깥 Transfer가 B를 target에 설치한다. C의 object/allocator/destroy pointer는 반환·소멸 없이 사라진다.
5. 최종 target의 B를 Reset해도 C allocation 및 그 capture가 남는다. SystemAllocator의 live ledger도 0으로 돌아오지 않는다.

ModuleLease도 old A의 release callback이 pre-acquired C를 같은 target에 move하면 동일하다. 바깥 assignment가 C를 덮어써 C의 release가 영구 누락된다.

**최소 수정:** old 상태를 지역 임시 소유자로 분리하고 incoming을 target에 먼저 설치한다. 그 뒤 old 임시를 파괴한다. 외부 destructor/release callback이 반환한 후 target 상태를 다시 덮어쓰지 않는다. 또는 재진입 변경을 명시 금지하고 항상 유효한 fail-fast guard를 제공할 수 있으나, 현재 Reset의 재진입 처리 방향과는 앞선 방식이 자연스럽다.

**검증:** A/B/C의 독립 constructed/destroyed 또는 retain/release 수를 기록한다. old destructor/release가 prebuilt C를 target에 재진입 설치하도록 구성하고, 마지막 reset 뒤 세 소유권이 모두 닫히는지 검사한다. 자기 move assignment 및 단순 Reset 재진입도 함께 검사한다. callable 파괴 중 새로운 Function이 설치된 뒤 외부 assignment가 소유권을 유실하면 실패다.

## 3. IC02 — 실패 진단이 stdio lock을 기다림

**위치:** `Engine/Core/Private/Diagnostics.cpp:6–9`; `Engine/Core/Public/Core/Assert.h:9–14`.

AssertFailed는 fprintf(stderr)와 fflush(stderr)를 호출한 뒤 abort한다. 이는 최하위 실패 경로가 최소 고정 buffer와 OS 출력만 사용하는 설계 문서 §5.1의 경계와 다르다. 일반 printf 계열 호출은 CRT stream 동기화와 내부 상태를 사용하므로 실패한 allocator/동기화 경로의 안전한 대체 경로가 아니다.

**실행 순서:** native thread A가 stderr의 CRT stream lock을 보유한 채 B 종료를 기다린다. B가 ROCKS_ASSERT 실패에 도달한다. B는 AssertFailed의 fprintf에서 A의 stream lock을 기다리고, A는 B 종료를 기다려 abort까지 도달하지 못한다. macOS flockfile, Windows CRT stream lock을 이용한 자식 프로세스 fixture로 구성할 수 있다.

**최소 수정:** 고정 stack buffer에 expression/file/line을 제한 길이로 작성한 뒤 Platform의 최소 fatal adapter에 전달한다. stdio·동적 할당·일반 logger·scheduler를 호출하지 않는다. 숫자 변환과 문자열 자르기도 bounded 연산으로 한다. 한 메시지가 길다고 heap formatting을 재시도하지 않는다.

**검증:** stderr CRT lock을 다른 native thread가 보유한 자식 프로세스에서 assertion을 실행하고 timeout 전에 비정상 종료하는지 검사한다. 정상 assertion과 매우 긴 파일/표현식도 별도 검사한다. 실패 진단을 위해 allocator를 다시 호출하지 않는 negative allocator fixture를 추가한다.

## 4. IC03 — 테스트 실패 집계의 native thread data race

**위치:** `Tests/Foundation/Main.cpp:10`, `Main.cpp:18–20`, `Main.cpp:137–143`; `Tests/Support/Test.h:30–43`.

RecordFailure의 `++failures`는 비원자 plain uint64 갱신이다. CHECK/REQUIRE는 호출 thread를 제한하지 않는다. 두 native worker에서 CHECK(false)를 실행하면 같은 counter를 동기화 없이 수정한다. failure 수가 유실될 수 있고 C++ data race 자체가 발생한다. 이것은 Foundation의 동시성 테스트 결과 신뢰성을 떨어뜨린다.

현재 작성된 PlatformTests는 worker-local 상태를 모아 join 뒤 owner에서 CHECK하는 방식으로 이를 피한다. 따라서 현재 test run에서 반드시 발생한다고 단정하지 않는다. 공통 하네스를 사용하는 자연스러운 native-thread fixture의 안전성 결함이다.

**최소 수정:** RecordFailure 집계를 Platform mutex 또는 검증된 원자 primitive로 보호한다. worker가 종료·join되기 전에 해당 테스트의 집계를 확정하지 않는 규칙을 명시한다. 또는 worker-local failure sink를 제공하고 join 뒤 합산한다. 단순히 volatile을 붙이는 수정은 허용하지 않는다.

**검증:** 자식 프로세스에서 N개 native thread가 각각 M번 CHECK(false)를 실행하고 join한다. exit 실패와 정확한 N×M count를 검사한다. 1/2/N thread 및 반복 실행, 지원 target의 TSan을 별도 기록한다. 기존 check/require negative control은 단일 호출만 검사하므로 이 경합을 검출하지 않는다.

## 5. IC04 — CPU scope 이름의 truncation 정보 유실

**위치:** `Engine/Core/Private/Diagnostics/ProfileScope.cpp:8–15`, `ProfileScope.cpp:18–20`; `Engine/Core/Private/Diagnostics/BootstrapLog.cpp:29–35`; `Tests/Foundation/HarnessTests.cpp:29–45`.

ProfileScope는 이름을 name_[120]에 먼저 자르고 null-terminate한다. 이후 LogBuffer::Write에는 이미 길이가 119 이하인 문자열만 전달하므로 Write는 truncated=false로 기록한다. 실제 긴 이름이 잘렸는데도 begin/end 모두 완전한 이름으로 보고된다. 동일한 앞 119자를 가진 다른 scope의 구분도 불가능해진다.

**최소 수정:** ProfileScope가 source truncation 여부를 저장하고 LogRecord의 truncated flag에 전달한다. 또는 scope 이름 복사를 동일한 LogBuffer bounded-copy 경로로 통합하면서 source truncation을 보존한다. scope 식별자를 추가한다면 이름 잘림과 identity를 분리하되, 첫 수정에 대형 profiler 기능을 요구할 필요는 없다.

**검증:** 119/120/200-byte 이름과 앞부분이 같고 뒷부분만 다른 두 이름으로 ProfileScope를 구성한다. begin/end의 문자열 종단과 truncated flag가 정확한지 검사한다. 기존 긴 이름 테스트는 LogBuffer::Write를 직접 호출하므로 ProfileScope의 선행 잘림을 검사하지 않는다.

## 6. 확인한 정상 경계와 범위

- Function::TryCreate는 allocation 성공 전 callable을 이동하지 않으므로 allocation 실패의 caller 소유권 보존 방향이 맞다. Move/Reset 자체의 내부 allocation은 없으며 allocator의 선행 수명을 요구한다.
- InlineExecutor::Submit는 observer record를 먼저 확보한 뒤 body를 이동한다. body와 capture cleanup이 끝나고 terminal 상태를 기록하므로, 오래 보관한 완료 handle이 실행 capture를 붙잡는 문제는 없다.
- InlineExecutor는 단일 owner·동기 실행 subset임을 공개 header에 명시한다. worker/DAG/cancellation 미구현을 이 범위의 오류로 재분류하지 않았다. TaskHandle의 WrongThread 및 copy/reset owner assertion도 이 경계에 부합한다.
- ModuleLease의 일반 copy/move/Reset는 retain/release 균형을 유지하는 구조다. 다만 동적 module unload 시 code thunk 자체의 수명과 retain/release provider의 thread safety는 향후 C27에서 별도 검증이 필요하다. 실제 DLL unload가 구현·검증되었다고 간주하지 않는다.
- LogBuffer는 단일 owner 고정 capacity capture이며 overflow를 CapacityExceeded와 Dropped로 표시한다. 다중 producer나 offline viewer 완료를 주장하지 않는다.
- CMake는 Base→Platform→Core→Task/Object 계층과 예외·RTTI 비활성, public header 단독 compile target을 제공한다. audit_runtime.py는 주석에 lexical guard임을 명시하며 transitive binary dependency 증명으로 오인하지 않아야 한다.
- CI 설정의 Windows/macOS/ASan job 정의는 실제 실행 증거가 아니다. compiler/SDK 버전과 실행 로그를 최종 결과에 함께 보존한다.

## 7. 소유 코드에 대한 다른 검토자의 발견 처리

C00/C06 작성자는 `Span<Derived>→Span<Base>` 변환으로 배열 stride가 달라지는 C01–C05 결함을 지적하였다. 본 검토자는 자기 소유 파일에 한하여 `U(*)[]→T(*)[]` compatibility로 Span 변환 및 pointer/count 생성자를 제한하였다. nullptr+0 경로와 mutable→const 변환은 유지하고 Derived Span·Derived pointer의 두 경로를 금지하는 compile-time 회귀를 추가하였다.

수정 파일은 `Engine/Core/Public/Core/Containers/Span.h`, `Tests/Core/CoreTests.cpp`이다. 이 수정의 실제 build/test 결과는 후속 빌드 증거로 확인해야 한다. 다른 작성자의 production 소스는 이 보고서에서 직접 수정하지 않았다.
