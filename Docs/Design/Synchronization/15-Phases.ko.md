## 15. 구현 단계

단계 기호는 SY다. 크기 S·M·L은 상대값이다.

| 단계 | 내용 | 선행 | 크기 |
|---|---|---|---|
| SY0 | PlatformBase: `CpuPause`·`SpinBackoff`, `WaiterSignal`(Windows `WaitOnAddress`·Android futex·macOS 조건 변수)과 가짜 OS 대기 seam, `PlatformBase.Build.cs` Windows `SystemLibraries = ["Synchronization"]`, 정책 허용 목록에 pause builtin, `Atomic<T>`·`AtomicRef<T>`(순서 기본값 없음)와 보조 연산(11절), `Base/Compiler.h`의 thread safety 매크로, `ActionGraph.cs` 430·471–472행에 `-Wthread-safety`, MetaCompiler `--clang-arg` 허용 목록과 회귀 검사, 시험. **통과 조건**: MetaCompiler 지정자 파서가 필드 형식 `Atomic<...>`·`AtomicRef<...>`를 리플렉션 struct 표시 `Atomic`과 혼동하지 않음을 시험으로 확인 | 없음 | M |
| SY1 | `ParkingLot`(콜백 있는 `WakeMultiple`·`WakeAll` 포함)·`detail::QueueMutex`·영구 선형 할당·침투 목록 깨우기·스레드 레코드 수명과 TLS 파괴 뒤 임시 레코드(D31), `SyncHooks` 자리와 훅 호출 지점(10.4절), `SyncTestAccess.h`, 결정적 시험(14.2절), 단계 스케줄러 하네스(14.3절) | SY0 | L |
| SY2 | **제자리 교체**: 기존 `Platform/Sync.h`의 `Mutex`·`ConditionVariable`·`LockGuard`를 parking lot판으로 바꾼다(이름·호출 모양 그대로, constexpr·trivially destructible). `IntrusiveMutex`·`InitiallyLocked`·가드 다섯, `WaitResult::Failed` 제거와 그 사용처 정리, 대기 표시 경쟁 시험. 새 타입에 thread safety 주석 | SY1 | M |
| SY3 | `RecursiveMutex`·`ReadWriteMutex`·`ReadLockGuard`·`ReadLockOwner` | SY2 | M |
| SY4 | `ManualResetEvent`·`Semaphore`(기존 타입 제자리 교체), `AutoResetEvent`·`EventCount`·`OnceFlag`·`CallOnce`·`CallOnceUntilSuccess` | SY1 | M |
| SY5 | 진단: 보유 스택·순위(`LockRank`·`RankedMutex`, lane 구간), 읽기 재진입·잠금 쥔 채 대기·task 대기 검사, `InitiallyLocked` 예외, Core 로그 훅 설치, `sync.dump` | SY2–SY4 | M |
| SY6 | 정리: `Platform/Sync.h`를 `Platform/Sync/*.h`로 헤더 분할하고 사용처 include 갱신, OS 잠금 wrapper 제거, 4.5절 1·2·4·6·7·8·9·12·13번 대체, 자유 원자 함수 호출을 `Atomic<T>`·`AtomicRef<T>`로 옮기고 정책 `atomic-detail` 켜기, 기존 필드 thread safety 주석, 전역 핵심 잠금·lane 잠금에 순위, 시험의 `YieldThread` 폴링을 `TestGate`로 | SY2–SY5 | L |
| SY7 | Core 동시 컨테이너: `IndexedFreeList`(`[태그 40 \| 색인 24]`, 로그·trace 블록 풀 통합), `FixedBlockPool`, `BoundedSpscQueue`, `IntrusiveMpscQueue`, `TakeAllStack`(`FrameArena` 소멸자 기록 포함), 모든 컨테이너의 `StepPolicy` 인자, interleaving 시험 | SY0, SY1(하네스), SY2(`FixedBlockPool`의 페이지 잠금) | M |
| SY8 | trace 채널 `lock`과 `SyncHooks` 설치(TraceCapture), TLS 버퍼 내보내기, 분석기 집계 | SY1, TraceSystem TR 단계 | S |
| SY9 | `ReclaimDomain`(SRCU, 동기화 대기 bit), 로그 설치·trace 생산자 대기 옮기기(4.5절 5번), 시험 | SY4, SY5(구간 안 대기 검사), SY7 | M |
| SY10 | `STRESS_TEST`·`--stress`, macOS `Tsan` 구성과 `__tsan_mutex_*` 주석, ARM64 stress 실행, 성능 측정 | SY2–SY9 | M |
| SY11 | 문서 반영: PlatformLayer.ko.md 68·270행, Docs/Implementation/PlatformConcurrency.ko.md 55행, UnrealCoreFeatureAudit.ko.md UE-C17 44행, LoggingSystem 8.4절(깨움·`WaitForSpace`), TraceSystem 4.1·채널 표, CVarSystem 콘솔 영역 `sync`, EngineImplementationPlan.ko.md 719행(lane 순위 구간과 task 대기 검사)·423행(Task 설계 후속 문서), GarbageCollectionScale GC-S4 블록 풀. 구현 문서, 묶음 끝 리뷰 | 전부 | S |
| SY12 | `RealtimeWakeSignal`(9.6절, 개정 3): 상태 원자·OS 수단 셋(Windows·Android·macOS Mach semaphore)·가짜 OS 대기 seam 연결·시험 | SY0 | S |

- 임시 이름공간을 두지 않는다(D28). 새 `Mutex`·`ConditionVariable`의 API는 기존 API의 상위집합이다. 생성자가 constexpr이 되고 파괴자가 없어지며 `TryLockFor`가 더해진다. 그래서 SY2에서 같은 이름으로 제자리 교체하고, 호출 코드는 바뀌지 않는다. 바뀌는 것은 `WaitResult::Failed` 사용처(비시험 28곳)뿐이다. 두 종류의 Mutex가 공존하는 기간이 없다.
- SY3·SY4의 새 타입은 처음부터 `Platform/Sync/*.h`에 둔다. SY6은 기존 `Platform/Sync.h`의 헤더 분할과 중복 제거만 맡는다.
- SY2와 SY6은 Core·Task·RenderBase·ObjectRuntime·Crypto·TraceCapture·Localization·Compression·Graphics backend를 건드린다(SY2는 재컴파일과 `WaitResult` 정리, SY6은 include·원자 호출 옮기기). 메인 트리 구현 에이전트 한 번에 하나 규칙에 따라 다른 구현과 겹치지 않는 시점에 한다.
