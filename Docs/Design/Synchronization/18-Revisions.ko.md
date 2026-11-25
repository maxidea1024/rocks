## 18. 개정 이력

- 개정 1(2026-11-18): 초안. UE 5.8.4 조사, Rocks 현황과 중복 10건, 배치·잠금·신호·원자·lock-free·회수·진단·시험 설계, 단계 SY0–SY11, 결정 D1–D28.
- 개정 2(2026-11-18): 교차검증 1차 반영, 결정 확정.
  - 정확성: ConditionVariable 대기 bit 경쟁(알림 유실)을 고치고 대기 표시 공통 규칙과 콜백 있는 `WakeMultiple`·`WakeAll`을 정했다(8.6·10.3절, D9·D29). AutoResetEvent·Semaphore·OnceFlag·ManualResetEvent·ReadWriteMutex·ReclaimDomain에 같은 규칙을 적용했다.
  - 계약: ReadWriteMutex 읽기 재진입 금지와 Debug 검출(D7), `InitiallyLocked` 해제 예외, task 대기 검사와 lane 순위(D21), AudioRT 잠금 금지(D27).
  - lock-free: ABA를 `[태그 40 | 색인 24]`와 태그 폭 static_assert로(D18), ReclaimDomain을 SRCU로 명시하고 동기화 대기 bit로 독자 추가 비용을 없앴다(D17).
  - 기반: TLS 파괴 뒤 임시 레코드(D31), scratch 대신 침투 목록(D5), `SpinBackoff`(D8), 훅 호출 지점과 재진입 guard(D22·D30), `Atomic<T>`·`AtomicRef<T>` 한 계열과 순서 기본값 제거(D15), Semaphore 크기·최대값 범위(D12), `CallOnceUntilSuccess`(D13), `FetchUpdate`.
  - 시험: `StepPolicy`·TestAccess 설치로 `ROCKS_TESTING` 정책과 맞춤(D24), 가짜 OS 대기로 시한 시험, 대기 표시 경쟁 시험, 대기 bit 해제 시점 기대값, TLS 파괴 뒤 대기 시험.
  - 빌드·도구: `Synchronization` 시스템 라이브러리, thread safety 플래그 자리와 ThirdParty 제외, MetaCompiler 허용 목록(D20·D32), TSan의 Windows 거부 실측과 fence 한계(D25).
  - 단계: 임시 이름공간 없이 SY2 제자리 교체(D28), thread safety 매크로를 SY0으로, SY7 선행에 SY2, MetaCompiler `Atomic<...>` 확인을 SY0 통과 조건으로.
  - 사실 정정: UE ParkingLot `Reserve` 시점·재진입 검사 줄·scratch 할당기·TLS 파괴 경로, `FAAArrayQueue` 사용처(`LocalWorkQueue.h`), 사용 수 재계산, `EV` 36행, `TQueue`·`TCircularQueue` 제외 목록, IntrusiveMutex 대기 bit 해제 시점. Rocks 쪽은 D3D11·D3D12 줄 번호, TraceCapture Mutex 파일 수, `DeviceOwnership` 분류, RenderBase 인용 줄, 빠진 중복(`FrameArena`, `LogSystem` 소비자 spin·`WaitForSpace`, `TraceEvents` double 더하기, 시험의 폴링 반복).
  - Task 설계 문서가 없음을 적고 work-stealing·overflow·oversubscription을 "Task 설계(후속 문서)"로 남겼다(D33).
- 개정 3(2026-11-22): 실시간 깨우기 `RealtimeWakeSignal`(9.6절, D34, 단계 SY12)과 6절 대응표 한 줄. [오디오 시스템](../AudioSystem.ko.md) AD26·PR21의 요구다.
