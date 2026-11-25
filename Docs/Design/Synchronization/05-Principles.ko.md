## 5. 원칙

- **잠금이 기본이다.** 정확성이 먼저다. lock-free는 측정으로 이득이 보이고, 12절의 회수·ABA 규칙과 14절의 interleaving 시험을 통과한 곳에만 쓴다(`EngineImplementationPlan.ko.md` 423·768행과 같다).
- **작고 상수 초기화되는 동기화 객체.** 모든 잠금·이벤트는 constexpr 생성자와 trivially destructible이다. 전역 생성자 없이 `constinit` 전역으로 둘 수 있어야 한다. 그래야 spin 잠금을 따로 만들 이유가 없다(4.5절 1·8번).
- **OS별 코드는 대기 수단 하나로 줄인다.** 잠금·이벤트 알고리즘은 공용 코드 한 벌이다. 플랫폼마다 다른 것은 "한 스레드가 잠들고 다른 스레드가 깨운다"뿐이다(7.1절).
- **push 경로에서 할당하지 않는다.** lock-free 컨테이너는 초기화 때 용량을 잡거나 호출자가 노드를 준다. 실패는 `Result`·`false`로 드러내고 버리지 않는다.
- **최대값과 "없음"은 이름으로.** 시간 무한은 `InfiniteTimeout`(= `NumericLimits<uint32>::Max()`), 색인 없음은 `IndexNone` 계열, 선택 값은 `Optional`이다.
- **시간 운에 기대지 않는다.** 구현에는 시간으로 순서를 맞추는 코드가 없다(spin 한도는 횟수다). 시험은 sleep 없이 gate·훅으로 순서를 만들고, 시한은 시계와 OS 대기를 함께 가짜로 바꿔 확인한다(14절).
- **도는 반복은 끝이 있다.** 값이 바뀔 때까지 도는 곳은 `CpuPause`를 정해진 횟수만 하고 그 뒤 `YieldThread`나 대기로 넘어간다(`SpinBackoff`, 8.5절). 상대가 선점되어 멈춘 경우에도 CPU를 태우지 않는다.
- **순서는 RMW로 만든다.** StoreLoad 순서가 필요한 곳은 단독 fence 대신 RMW(`FetchAdd(0)`·`Exchange`)로 만든다. ThreadSanitizer가 단독 fence를 모델하지 못하기 때문이다(14.5절).
- **STL·외부 라이브러리 없음.** 원자는 `Atomics.h`의 Clang builtin, OS 대기는 OS API다. `std::atomic`·`std::mutex`는 쓰지 않는다.
