## 17. 미확인 사실

UE 쪽:
- `FSharedRecursiveMutex`의 재진입 공유 잠금 알고리즘 세부(`Private/Async/SharedRecursiveMutex.cpp` 387행)를 읽지 않았다.
- `Private/Unix/`·`Private/Linux/` 수동 리셋 이벤트 구현(futex 여부)을 읽지 않았다. Rocks는 Linux를 대상으로 하지 않는다.
- 작업 scheduler `FWaitingQueue`의 `Park`·`CommitWait`(spin·대기 cycle 수)를 읽지 않았다(`Public/Async/Fundamental/WaitingQueue.h` 75·102행).
- 잠금 순서 검사기·교착 검출기는 `Public/Async`·`Private/Async`·`Public/HAL`·`Private/HAL`에서 문자열 검색으로 찾지 못했다. 다른 모듈(예: 진단 도구)에 있을 수 있다.
- UE race detector(`Private/Sanitizer/RaceDetectorWindows.cpp`)가 어떤 컴파일러 계측을 요구하는지 읽지 않았다.
- 3.11절 사용 수는 문자열 하한이다. namespace 안 무접두 사용은 빠졌다.
- 렌더 스레드 "지연 기록 명령 목록"(`RenderCore/Public/RenderingThread.h` 725–740행)의 lock-free 구현 방식은 다시 읽지 않았다(`Docs/Design/RenderBase` 5.1절 인용만).

Rocks·도구 쪽:
- Android NDK의 ThreadSanitizer 지원 범위를 확인하지 못했다(확신 낮음, 공식 지원이 아닌 것으로 안다).
- macOS `WaiterSignal`의 "깨우는 쪽 `pthread_mutex_unlock` 직후 대기자가 노드를 파괴" 경로가 Apple libpthread에서 안전하다는 것은 POSIX의 "풀린 mutex 파괴 허용" 규정에 기댄 판단이고 실기에서 시험하지 않았다(7.3절, 확신 낮음).
- `thread_local` 객체의 파괴자 사용(ParkingLot 스레드 레코드 반환)이 Rocks 스레드 종료 규약(`Docs/Implementation/PlatformConcurrency.ko.md`의 TLS 소멸자 관련 시험)과 어긋나지 않는지 SY1에서 시험으로 확인한다(D31).
- parking lot `Mutex`와 SRWLOCK·`pthread_mutex_t`의 성능 비교는 측정하지 않았다. UE가 바꾼 근거를 따른 것이다.
- Android futex 대기의 `FUTEX_WAIT_BITSET_PRIVATE` 절대 시한은 `FUTEX_CLOCK_REALTIME`이 없으면 `CLOCK_MONOTONIC` 기준이다(futex(2) 규정, UE `AndroidPlatformManualResetEvent.cpp` 83–84행도 그렇게 쓴다). 대상 API 26 실기에서는 시험하지 않았다.
- lane 순위 구간의 순서(D21)는 계획 문서의 lane 정의에 맞춘 권장이다. 실제 lane 사이 잠금 순서는 SY5·SY11에서 사용처를 보며 확정한다.

해소된 항목(개정 2): ParkingLot 영구 할당 크기 근거(`PLC` 61행), `Synchronization.lib` 링크 자리(`SystemLibraries`), clang-cl의 `-Wthread-safety` 동작과 `-fsanitize=thread` 거부(로컬 실측), MetaCompiler `Atomic<...>` 확인(SY0 통과 조건으로 옮김).
