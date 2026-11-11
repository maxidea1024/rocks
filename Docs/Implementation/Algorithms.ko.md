# Core 정렬과 이진 탐색

`Core/Algorithms.h`는 연속 범위의 비안정 정렬과 이진 탐색을 제공한다. `Array.h`를 사용하면 배열을 직접 정렬할 수 있다.

```cpp
rocks::Array<int> values(allocator);
// TryPushBack 등 할당 가능한 작업의 실패는 호출자가 처리한다.
values.Sort();
values.Sort([](int a, int b) noexcept { return a > b; });

// 탐색은 같은 비교기로 이미 정렬된 범위에 적용한다.
const rocks::usize index = rocks::algo::LowerBound(values.View(), 42,
    [](int a, int b) noexcept { return a > b; });
```

`Sort`는 추가 메모리 할당 없이 최악 O(n log n)으로 정렬하며 동등한 원소의 기존 순서를 보장하지 않는다. 원소는 `noexcept` 이동 생성과 소멸을 지원해야 하며 이동 대입은 필요 없다. 메모리 주소는 유지되나 원소는 이동·파괴·재생성되므로 같은 위치에 예전 값이 남는다고 가정하지 않는다. 원소 내부에 저장된 자기 참조도 보정하지 않는다. 비교기는 `noexcept` 엄격 약순서를 제공해야 하고 배열·범위를 변경하거나 정렬에 재진입해서는 안 된다. 외부 동기화 없이 같은 범위를 동시에 읽거나 쓰면 안 된다.

포인터 원소는 자동 역참조하지 않는다. UE 5.6 `TArray::Sort`의 자동 역참조와 다른 규약이다. 서로 무관한 객체의 포인터 기본 비교는 이식 가능한 전체 순서를 제공하지 않으므로 대상 값을 비교할 때는 명시적 비교기를 쓴다. 비교기는 null 허용 여부와 대상 수명을 책임진다.

`LowerBound`와 `UpperBound`는 각각 동등 구간의 처음과 끝 위치를 반환하며, 삽입 위치가 끝이면 `Size()`를 반환한다. `BinarySearch`는 첫 동등 원소의 위치를 반환하며 없으면 `algo::NotFound`를 반환한다. 탐색 비교기와 반환값의 bool 변환은 `noexcept`여야 한다. 다른 타입의 탐색 키는 양방향 비교가 가능한 비교기를 명시한다. 범위, 키와 비교기가 빌린 데이터는 호출 종료까지 살아 있어야 한다.

안정 정렬, projection 기반 검색·정렬, 불연속 컨테이너의 범용 알고리즘은 아직 제공하지 않는다. 전체 UE Core 알고리즘에 대한 동등성이나 플랫폼별 성능을 의미하지 않는다.
