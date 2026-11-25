## 11. 원자 값

### 11.1 Atomic<T>

```cpp
namespace rocks::platform {

// 다른 스레드와 공유하는 값 하나다. 모든 접근은 원자적이고 메모리 순서를 늘 템플릿 인자로 적는다
// (기본값이 없다). 연산자 overload(=, ++, 암시적 변환)는 두지 않는다: 순서가 코드에 드러나야 하고,
// 실수로 일반 읽기·쓰기를 섞지 않게 한다.
//
// 사용 예:
//   Atomic<uint64> dropped;
//   dropped.FetchAdd<MemoryOrder::Relaxed>(1);
//   if (refs.FetchSub<MemoryOrder::AcquireRelease>(1) == 1) Destroy();
//   while (ready.Load<MemoryOrder::Acquire>() == 0) { backoff.Pause(); }
template <class T>
class Atomic {
public:
    constexpr Atomic() noexcept = default;          // 0 초기화
    constexpr explicit Atomic(T initial) noexcept;
    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    template <MemoryOrder Order> T Load() const noexcept;
    template <MemoryOrder Order> void Store(T value) noexcept;
    template <MemoryOrder Order> T Exchange(T value) noexcept;
    template <MemoryOrder Success, MemoryOrder Failure>
    bool CompareExchangeWeak(T& expected, T desired) noexcept;
    template <MemoryOrder Success, MemoryOrder Failure>
    bool CompareExchangeStrong(T& expected, T desired) noexcept;
    template <MemoryOrder Order> T FetchAdd(T value) noexcept;  // 정수만, 이하 같음
    template <MemoryOrder Order> T FetchSub(T value) noexcept;
    template <MemoryOrder Order> T FetchAnd(T value) noexcept;
    template <MemoryOrder Order> T FetchOr(T value) noexcept;
    template <MemoryOrder Order> T FetchXor(T value) noexcept;

private:
    alignas(sizeof(T)) T value_{};
};

// 이미 있는 T 객체 하나를 원자로 본다. 객체는 AtomicRef보다 오래 살고 T의 자연 정렬을 지켜야 한다
// (Debug는 정렬을 검사한다). 같은 객체를 AtomicRef로 보는 동안 다른 스레드가 일반 접근을 하면 안
// 된다. 멤버와 순서 규칙은 Atomic<T>와 같다.
//
// 사용 예:
//   AtomicRef<uint64>(cell.sequence).Store<MemoryOrder::Release>(next);   // trivially copyable 칸
template <class T>
class AtomicRef {
public:
    explicit AtomicRef(T& object) noexcept;
    // Atomic<T>와 같은 멤버
};

} // namespace rocks::platform
```

- 대상 타입·순서 제약은 지금 자유 함수와 같다(1·2·4·8 byte 정수·열거형·포인터·bool, lock-free 필수, 포인터 산술 금지, 잘못된 순서 조합은 static_assert).
- 크기·정렬은 T와 같다. `alignas(sizeof(T))`로 8 byte 값이 구조체 안에서 4 byte 정렬로 떨어지는 일을 막는다.
- **순서 기본값 없음.** 개정 1은 "순서를 늘 이름으로 적는다"고 하면서 기본값(SequentiallyConsistent)을 두어 모순이었다. 기본값을 없앤다. 순서를 빠뜨리면 컴파일 오류다.
- **한 계열(D15).** 공개 원자 API는 `Atomic<T>`(값을 소유)와 `AtomicRef<T>`(기존 객체를 봄) 둘이다. 지금의 자유 함수(`AtomicLoad` 등)는 `platform::detail`로 내려 이 두 타입의 구현으로만 쓴다. `__atomic_*` builtin은 여전히 `Atomics.h` 한 곳이다.
  - `AtomicRef`가 자유 함수 자리를 맡는다. trivially copyable이어야 하는 칸(`BoundedMpscQueue::Cell`), 외부·공유 메모리, 원자 접근과 일반 접근이 시기별로 나뉘는 값(초기화 때 일반 쓰기, 게시 뒤 원자 읽기)이 그 경우다.
  - 늘 원자로만 접근하는 필드(통계 카운터, 상태 word, 참조 계수, 게시 포인터)는 `Atomic<T>`로 둔다.
  - 정책 검사 `atomic-detail`을 더한다. `platform::detail::Atomic*`을 `Platform/Atomics.h` 밖에서 부르면 위반이다. 기존 호출은 SY6에서 모두 `Atomic<T>`·`AtomicRef<T>`로 옮기고, 그 뒤 검사를 켠다.
- 이름 확인: 리플렉션의 struct 표시 `Atomic`(`ObjectRuntime/Tests/Reflection/AtomicStructTests.cpp` 7–10행)은 enum 값·지정자라 C++ 타입 이름과 겹치지 않는다. MetaCompiler 지정자 파서가 필드 형식 `Atomic<...>`·`AtomicRef<...>`를 지정자로 오인하지 않는지는 SY0의 통과 조건으로 확인한다(15절).

### 11.2 CpuPause와 SpinBackoff

- `Atomics.h`에 `void CpuPause() noexcept`를 둔다. x86-64는 `__builtin_ia32_pause()`, ARM64는 `__builtin_arm_isb(15)`(UE와 같은 `isb`)다. 정책 `compiler-builtin`의 `Atomics.h` 허용 목록에 이 두 builtin을 더한다.
- `YieldThread()`(OS 양보)와 이름으로 구분한다.
- `SpinBackoff`(D8): 반복 하나의 상태(횟수)다. `Pause()`는 처음 `PauseLimit`번(기본 40)은 `CpuPause()`, 그 뒤로는 `YieldThread()`다. 시간이 아니라 횟수로 넘어간다. 잠금 밖에서 "값이 바뀔 때까지 짧게 도는" 모든 반복이 이것을 쓴다(8.5절).

```cpp
// 사용 예(소비자가 미공개 칸을 기다린다):
//   SpinBackoff backoff;
//   while (!cell.IsPublished()) { backoff.Pause(); }
class SpinBackoff {
public:
    static constexpr uint32 PauseLimit = 40;
    void Pause() noexcept;
private:
    uint32 count_ = 0;
};
```

### 11.3 보조 연산

4.5절 4·7·10번을 모은다. `Atomics.h`의 `Atomic<T>`·`AtomicRef<T>` 멤버다. 순서는 다른 멤버처럼 템플릿 인자로 적는다.

| 연산 | 뜻 | 대체하는 곳 |
|---|---|---|
| `FetchMax(value)` | 현재 값보다 크면 바꾼다. 이전 값을 돌려준다 | `TraceBuffers.cpp` 102–110행 |
| `TryFetchAddBounded(value, limit)` | 더해도 `limit` 이하일 때만 더한다. 성공이면 `Optional<T>`(이전 값), 아니면 빈 값 | `TraceBuffers.cpp` 189–200행, `DynamicLibrary.cpp` 185–194행 |
| `FetchAddSaturating(value, limit)` | 넘으면 `limit`에 멈춘다 | `Archive.cpp` 242–262행 |
| `TryIncrementIfNonZero()` | 0이 아닐 때만 1 올린다(약한 참조 승격) | `SharedPtr.h` 52–62행 |
| `TakeUniqueNumber()` | 0을 쓰지 않는 단조 번호를 CAS로 하나 받는다. 소진되면 빈 `Optional`(호출자가 Fatal 또는 오류) | `StackAllocator.cpp` 13–20행, `TypeRegistry.cpp` 18–25행, `Name.cpp` 9–20행 |
| `FetchUpdate(function)` | 현재 값에 함수(`T → T`, noexcept)를 적용한 결과를 CAS 반복으로 쓴다. 이전 값을 돌려준다. 함수는 부작용이 없어야 한다(여러 번 불릴 수 있다) | `TraceEvents.cpp` 133–139행(double을 `uint64` bit 표현으로 더하기) |

- 원자 참조 계수 전용 타입(UE `TAtomicRefCount`)은 두지 않는다. UE 타입의 실질은 AutoRTFM 처리이고(2.3절), Rocks에서는 `Atomic<uint32>` + `FetchSub<AcquireRelease>` + 위 보조 연산이면 된다(D16).
