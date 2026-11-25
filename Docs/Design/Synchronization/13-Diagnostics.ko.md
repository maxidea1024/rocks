## 13. 디버그 지원

### 13.1 정적 검사: Clang thread safety analysis

- `Base/Compiler.h`에 `ROCKS_CAPABILITY(name)`, `ROCKS_SCOPED_CAPABILITY`, `ROCKS_GUARDED_BY(lock)`, `ROCKS_PT_GUARDED_BY(lock)`, `ROCKS_REQUIRES(...)`, `ROCKS_REQUIRES_SHARED(...)`, `ROCKS_ACQUIRE(...)`, `ROCKS_RELEASE(...)`, `ROCKS_TRY_ACQUIRE(...)`, `ROCKS_EXCLUDES(...)`, `ROCKS_NO_THREAD_SAFETY_ANALYSIS`를 둔다(Clang `__attribute__((capability))` 계열).
- `Mutex`·`RecursiveMutex`·`ReadWriteMutex`는 capability, 가드는 scoped capability다.
- 빌드에 `-Wthread-safety`를 켜고 경고를 오류로 둔다. Windows(clang-cl)를 포함한 모든 대상에서 컴파일 때 돈다. 실행 시간 운에 기대지 않는 유일한 경쟁 검사다.
  - **실측(2026-11-18, clang-cl 21.1.6).** clang-cl이 `-Wthread-safety`를 접두 그대로 받는다. `/W4 /WX`와 함께 주면 보호 변수 쓰기가 `error: writing variable 'v' requires holding mutex 'm' exclusively [-Werror,-Wthread-safety-analysis]`로 막힌다. `/W4`만으로는 켜지지 않는다.
  - **빌드 변경(SY0).** `Programs/Rocks.Build/ActionGraph.cs`의 Windows 인자(430행 `/W4`·`/WX` 옆)와 POSIX 언어 인자(471–472행)에 `-Wthread-safety`를 더한다. MetaCompiler는 네이티브 프로필 인자만 `--clang-arg`로 받는다. 허용 목록을 늘릴 때 AST 동등성 검토와 회귀 검사를 먼저 둔다는 규칙이 있다(`Build/README.md` 140행). 그래서 이 경고 인자를 허용 목록에 더하고 회귀 검사를 함께 둔다. 경고 인자라 AST는 바뀌지 않는다.
  - **범위(D32).** 외부 라이브러리(ThirdParty manifest로 빌드하는 모듈)에는 걸지 않는다. Rocks 모듈만이다.
- 매크로는 SY0에서 `Base/Compiler.h`에 둔다. 주석 달기는 SY2부터 새 타입·새 코드에 하고, 기존 필드는 SY6에서 옮기며 단다. 분석이 표현하지 못하는 곳(조건부 잠금, 잠금 넘겨주기, 상태 word 안의 `IntrusiveMutex`)은 `ROCKS_NO_THREAD_SAFETY_ANALYSIS`와 이유 주석이다(D20).

### 13.2 잠금 순위와 보유 스택(Debug·Development)

- **보유 스택.** 스레드마다 고정 16칸 배열(잠금 주소, 종류, 순위, 호출 위치)이다. 할당하지 않는다. 넘치면 Fatal이다.
- **검사.**
  - 같은 비재진입 잠금을 다시 잡음 → Fatal(자기 교착). `ReadWriteMutex`는 같은 스레드의 읽기 두 번, 읽기 뒤 쓰기, 쓰기 뒤 읽기를 모두 포함한다. 쓰기 대기자가 있을 때만 실제로 교착하는 읽기 재진입(8.4절)도 대기자 유무와 관계없이 결정적으로 잡는다.
  - `OnceFlag` 실행 중 같은 flag로 다시 부름 → Fatal(실행하는 동안 flag 주소를 종류 `Once`로 올린다, 9.4절).
  - 쥐지 않은 잠금을 풂, 다른 스레드가 쥔 잠금을 풂 → Fatal. 예외: `InitiallyLocked`로 만든 잠금의 첫 해제는 보유 스택에 없는 것이 정상이라 검사하지 않는다(8.1절).
  - 잠금을 쥔 채 기다림 → Fatal. 대상은 `ManualResetEvent`·`AutoResetEvent`·`EventCount`·`Semaphore`·`OnceFlag`(다른 스레드 실행을 기다림)·`Thread::Join`·`ReclaimDomain::Synchronize`, 그리고 task 대기(`WorkerScheduler`의 작업 완료 대기·`WorkerTaskEvent` 대기·`DedicatedLane` 완료 대기)다. task 대기는 `EngineImplementationPlan.ko.md` 719행 "lock 보유 상태의 task wait는 금지"를 따른다. `ConditionVariable::Wait`의 짝 잠금은 예외다. 의도된 경우는 `AllowWaitWhileLocked` 범위로 표시한다.
  - 순위 위반: 쥐고 있는 순위 이하의 순위를 잡음 → Fatal.
- **순위.** `enum class LockRank : uint16`을 PlatformBase `LockRank.h`에 한 곳으로 모은다(값 사이에 간격을 둔다). `RankedMutex<LockRank::Log>`·`RankedReadWriteMutex<...>`가 순위를 타입에 싣는다. 크기는 같은 1·4 byte다.
  - 계획 719행 "lane별 lock rank를 정하고 debug에서 검사한다"에 맞춘다. 순위 값은 lane 순서로 구간을 나눈다. 바깥부터 Game → Render → RHI·장치 → IO → worker 공용 순이고, 각 구간 안에 그 lane 소유 잠금을 둔다. 엔진 전역 핵심 잠금(로그 설치, trace 등록, 객체 레지스트리, 형 등록, 이름 풀, 할당기)은 모든 lane 구간보다 안쪽(가장 나중에 잡는 쪽)에 둔다. AudioRT는 잠금을 쓰지 않으므로 구간이 없다(D27).
  - lane 소유 잠금과 전역 핵심 잠금에 순위를 붙인다. 순위 없는 잠금은 순위 검사에서 빠지고 위의 다른 검사만 받는다.
  - 계획 719행의 문장과 이 구간 정의를 맞추는 문서 반영은 SY11에 넣는다.
- 동적 순서 그래프(Abseil 방식: 실제로 잡은 순서로 그래프를 만들고 순환을 찾는다)는 후속이다. 순위 없는 잠금 사이의 교착을 잡지만 메모리·비용이 크다(D21).
- Release 빌드는 이 검사를 모두 뺀다(빈 함수).
- 보고: Core가 설치한 훅이 로그(`Log::Fatal`, 영어 메시지)로 낸다. 설치 전이면 `platform::Fatal`이다. 메시지는 영어(로그·Fatal 문구 규칙)다.

### 13.3 경합 trace

- TraceSystem에 채널 `lock`을 더한다. 기록은 slow path에서만 낸다.
  - `Sync.ContendedWait`: 잠금 주소, 종류(Mutex·ReadWrite 읽기/쓰기·Recursive·Event·EventCount·Semaphore·Condition), 순위(있으면), 시작·끝 tick.
  - spin만으로 잡은 경우는 기록하지 않는다(빈도가 너무 높다). 통계 카운터(`lock.spins`·`lock.parks`)만 올린다.
- 구현: `SyncHooks.onWaitBegin/onWaitEnd` 함수 포인터다. 호출 지점은 10.4절이다. `onWaitBegin`은 bucket을 잡기 전에 부르고, 둘 다 스레드 지역 버퍼에만 쓴다. trace 세션으로 내보내기는 그 스레드가 ParkingLot 밖에 있을 때(다음 trace 블록 공개) 한다. trace 기록 경로가 잠금을 잡을 수 있기 때문이다(`TraceSnapshot.cpp`의 writer 잠금, slot registry, 이름 표 삽입 잠금). TraceCapture가 세션 시작 때 설치한다. Core·PlatformBase가 TraceCapture에 의존하지 않는다(`TraceCapture`는 Core에 의존, `TraceCapture/TraceCapture.Build.cs` 9행).
- 재진입 guard: 훅 안에서 다시 경합 대기가 생기면 그 대기는 기록하지 않는다(10.4절).
- 비용: 경합 없는 잠금은 0이다. 잠드는 경로에서 훅 포인터 relaxed load 하나와 분기 하나다. 잠드는 비용(μs 단위)에 비해 무시할 수 있다.
- 분석기(TraceAnalysis)는 주소별 대기 시간 합·최대, 순위별 집계를 보인다.

### 13.4 교착 진단

- **`sync.dump` 콘솔 명령**(CVar 콘솔 영역 `sync`): ParkingLot bucket을 하나씩 잠그며 잠든 스레드(스레드 이름·ID·대기 주소·대기 시작 tick)를 모으고, Debug면 각 스레드의 보유 스택을 붙여 로그로 낸다. 진단 출력이며 판정에 쓰지 않는다.
- **Fatal·crash 때**: 같은 내용을 TryLock으로만 모은다(못 잡은 bucket은 "잠김"으로 적는다). 멈춘 쪽을 기다리지 않는다.
- **watchdog은 두지 않는다.** "N초 이상 기다리면 교착"은 시간에 기대는 판정이다. 오래 기다림은 trace로 보고, 교착 판정은 사람이 `sync.dump`로 한다(D23).
