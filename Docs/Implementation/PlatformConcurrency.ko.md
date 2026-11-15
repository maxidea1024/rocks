# Rocks 플랫폼 독립 스레드·동기화 계약

기준일: 2026-11-10. PlatformBase의 저수준 기반을 구현한 범위이며 Task Graph·worker scheduler·병렬 GC 완료를 뜻하지 않는다.

## 경계와 파일 선택

공용 헤더는 `Platform/Thread.h`, `Platform/Sync.h`다. 공개 API에 Windows/POSIX 타입과 OS 조건부 전처리를 노출하지 않는다. CMake가 `Private/Win64` 또는 `Private/Mac` 번역 단위를 선택하고, 공통 상태·deadline·Event/Semaphore 구현은 `Private/Common`에 둔다. Core·Name·Localization·Object와 공통 테스트는 같은 API를 사용한다.

OS 헤더·호출은 플랫폼 private backend와 테스트용 OS adapter만 사용한다. SDK의 `Yield` 매크로와 충돌하지 않도록 공개 함수 이름은 `YieldThread`다. 공용 함수에 `#undef`를 늘려 문제를 가리지 않는다. 지원하지 않는 OS는 구성 단계에서 명확히 실패하며 Linux·콘솔 backend를 구현한 것으로 표시하지 않는다.

`PolicyChecks.PlatformBoundary`(`policy`)와 음성 fixture(`Rocks.ToolTests self`)는 공용 소스의 알려진 OS 헤더·조건부 전처리·네이티브 호출 유입을 검사한다. `PolicyChecks.TargetMacros`는 원시 컴파일러 대상 매크로를 `Base/Compiler.h` 밖에서 거부한다(백엔드도 `ROCKS_ARCH_*`·`ROCKS_PLATFORM_*`를 쓴다). 이것은 lexical guard이며 모든 native API 이름이나 매크로 확장을 증명하는 검사는 아니다. 새로운 backend를 추가할 때 플랫폼 선택·감사 범위·native CI를 함께 추가한다.

## API와 수명

| API | 계약 |
|---|---|
| `Thread::Start(entry, context)` | 비복사·비이동 Thread에서 실행 시작. 실패하면 새 join 소유권을 만들지 않음. null entry 거부. allocator·STL·Core 의존 없음 |
| `WaitForCompletion(timeoutMs)` | entry 반환을 기다림. timeout은 실행 취소나 소유권 반환이 아님. CRT/TLS 종료까지 완료되었다는 뜻이 아님 |
| `Join()` | native 스레드와 TLS 정리까지 회수하는 무기한 대기. 자기 자신은 SelfJoin, 진행 중 다른 Join은 Busy. 성공 후 재시작 가능 |
| `Mutex` / `LockGuard` | 비재귀 상호 배제와 scope 반환. 보호 중인 데이터의 의미와 lock 순서는 상위 계층 책임 |
| `ConditionVariable` | 소유 중인 Mutex를 원자적으로 풀고 대기한 뒤 다시 잡음. spurious wake를 허용하므로 predicate를 반복 검사 |
| `ManualResetEvent` | Signal 후 Reset 전까지 신호 유지, 여러 waiter 해제. Reset과 대기가 경합하면 predicate를 얻는 순서에 따름 |
| `Semaphore(initial, maximum)` | Wait 성공 시 token 1개 소비. Release(0) 또는 상한 초과는 전체 실패하며 count 불변. 공정성 보장 없음 |
| `SleepFor(ms)` / `YieldThread()` | 요청한 유한 휴지 / 실행 기회 양보. 동기화·정확한 스케줄 순서의 대체 수단이 아님 |

Thread의 entry/context 및 관련 자원은 Join 성공까지 유효해야 한다. joinable Thread를 파괴하면 Fatal로 수명 위반을 드러낸다. 암묵 detach·강제 종료·소멸자에서 숨은 무기한 join을 제공하지 않는다. 실패한 Join은 소유권을 남긴다. 모든 제어 호출이 복귀한 뒤 Thread를 파괴한다.

Thread 내부 gate가 Start/Wait/Join/IsJoinable의 상태를 보호한다. Join은 reservation을 잡은 뒤 gate 밖에서 native 종료를 기다린다. 이전 실행을 기다린 caller가 남아 있으면 새 Start를 Busy로 거부하여 완료 상태 재사용을 막는다. self-join 판별은 재사용될 수 있는 OS thread ID 대신 실행 중 ThreadState의 TLS identity를 사용한다.

Mutex·ConditionVariable·Event·Semaphore 자체를 다른 호출과 동시에 파괴하지 않는다. 닫힘·취소가 필요한 서비스는 상위 계층의 predicate와 drain 규약을 설계한다. native 초기화 실패와 프로그래머의 불가능한 semaphore 초기값은 Fatal이다. wait의 시스템 오류는 Failed로 반환한다.

## 시간과 backend 의미

대기 함수의 `InfiniteTimeout`은 무한을 뜻하며 0은 즉시 predicate 검사다. Event·Semaphore·Thread 완료 대기는 최초 monotonic deadline의 잔여 시간을 재계산하여 spurious wake마다 timeout을 다시 시작하지 않는다. timeout은 deadline 이후 즉시 스케줄될 보장이 없으며 오버런은 OS 스케줄링의 영향을 받는다.

Windows는 CRT 정리가 있는 `_beginthreadex`와 handle 회수를 사용한다. macOS는 pthread와 상대 시간 condition wait를 사용한다. 공통 `Join(timeout)`을 약속하지 않으며 완료 대기와 native 회수를 구분한다. `SleepFor`에는 무한 sentinel을 적용하지 않으므로 uint32 최댓값도 유한 시간이다.

## 검증과 후속 범위

단위 fixture는 thread start/재시작/self-join, 완료 timeout, CV predicate·notify, 이벤트 유지·reset, semaphore 상한·생산자/소비자, TLS 소멸자가 막힌 동안의 entry 완료와 Join 완료 분리를 검증한다. 미회수 Thread 파괴는 자식 프로세스에서 Fatal 진단을 확인한다. Name·Text·Object의 동시성 fixture도 이 공통 계층을 사용한다.

원자 연산 추상화, RWLock, thread name/priority/affinity/stack 설정, 취소 토큰, worker pool·dependency scheduler는 후속이다. native 스레드 생성·join의 OS 실패 강제 주입과 모든 스케줄 interleaving의 증명은 현재 범위가 아니다. 실제 실행 구성·개수·source digest는 Foundation 상태 문서에서 확인한다.

현재 구현은 동일 source digest로 Windows x64 Debug/Release/ASan과 macOS ARM64 Debug/ASan 실행을 통과했다. 동시 Join/Wait fixture 2개와 TLS 종료 분리도 전체 110개에 포함된다. 최종 clean commit 근거는 [CI 실행 42771207691](https://github.com/maxidea1024/rocks/actions/runs/42771207691)이며 전체 실행·미완료 범위는 [Foundation 상태](FoundationStatus.ko.md)를 따른다.
