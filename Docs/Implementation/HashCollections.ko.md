# Rocks 해시 컬렉션: 설계와 검증 기준

이 문서는 Core `HashMap`과 `HashSet`의 사용법, 안전 계약 및 후속 계획이다. 현재 구현 범위는 아래 예제와 공개 헤더를 기준으로 한다. 아래 우선순위 표의 완료 기준은 전체 목표이며 구현되지 않은 항목도 포함한다.

## 현재 API와 사용법

```cpp
rocks::HashMap<rocks::int64, rocks::String> players(allocator);
rocks::String playerName(allocator);
auto init = playerName.TryAssign("Rocks");
if (!init) {
    return init.Error();
}

// 삽입 키는 정확히 K 타입이다. int 리터럴을 int64 키로 쓸 때는 타입을 명시한다.
auto inserted = players.Insert(rocks::int64{1}, rocks::Move(playerName));
if (!inserted) {
    return inserted.Error();
}
if (inserted.Value() == rocks::InsertOutcome::AlreadyPresent) {
    // Insert는 기존 값을 바꾸지 않는다. 교체가 필요하면 InsertOrAssign을 사용한다.
}

if (auto* name = players.Find(rocks::int64{1})) {
    // name은 빌린 포인터다. 아래 무효화 규약을 지켜야 한다.
    (void)name;
}

players.ForEach([](const rocks::int64& key, rocks::String& name) noexcept {
    // key는 읽기 전용이다. name은 변경할 수 있다.
    (void)key;
    (void)name;
});
```

예제는 `ErrorCode`를 반환할 수 있는 함수 내부를 가정한다. `Insert`와 `HashSet::Add`는 기존 항목을 보존하고 `Inserted` 또는 `AlreadyPresent`를 반환한다. `InsertOrAssign`은 기존 값을 교체하며 기존 키면 `AlreadyPresent`다. 결과 객체의 조건문은 **연산 성공 여부**이며 삽입 여부는 반드시 `Value()`와 enum을 비교한다. 중복 보존 삽입과 실패는 전달한 이동 인자를 소비하지 않는다.

`Find`, `FindOrAdd`, `FindOrAddWith`, `Contains`, `Erase`, `EraseIf`, `Count`, `Capacity`, `IsEmpty`, `Reserve`, `Compact`, `Clear`, `Reset`, `ForEach`를 제공한다. Set의 `Find`는 저장된 키의 읽기 전용 포인터다. `HashMap::FindStoredKey`도 읽기 전용 키 포인터를 제공한다. `Clear`는 저장소를 유지하고 `Reset`은 반환한다. `Compact`는 삭제 표시를 제거하고 적정 용량으로 축소하며, 실패하면 내용·용량·기존 포인터를 보존한다.

기본 해시는 정수·enum·`String`·`StringView`·`Name`·`Guid`를 지원한다. `HashMap<String, V>`와 `HashSet<String>`은 `StringView`로 할당 없이 조회할 수 있다. 외부 정책으로 이종 조회를 확장할 때도 동등한 키는 같은 해시여야 한다. 키를 삽입할 때에는 정확한 `K` 타입을 사용한다. `StringView`의 원본은 키가 저장된 동안 유효하고 내용이 바뀌지 않아야 한다.

`Reserve(n)`의 단위는 원소 수다. 삭제 표시가 많이 쌓인 경우에는 용량이 충분해도 후속 삽입이 같은 크기로 재해시할 수 있으므로, 사전 예약이 이후 삽입의 무할당이나 포인터 유효성을 보장하지 않는다. 재해시가 실제로 발생하면 모든 원소 포인터가 무효화된다. 삭제는 해당 원소만, `Clear`는 모든 원소를 무효화한다. 값 교체도 해당 값 포인터를 무효화한다.

순회 중 중첩 조회와 순회, Map의 값 수정은 가능하다. 구조를 바꾸는 삽입·예약·압축은 `Busy`, 삭제·`Clear`·`Reset`·이동·소멸은 Fatal이다. 콜백은 `noexcept`여야 한다. 신규 삽입과 값 교체는 저장소 원소의 직접 별칭을 거부한다. key/value 인자가 서로 겹칠 때는 두 생성이 모두 trivial인 경우에만 허용한다. 중복 `Insert`는 입력을 소비하지 않고 성공한다. 간접 참조 관계는 자동으로 모두 탐지할 수 없다.

Rocks는 자체 `Allocator`, `String` 계층과 C++23을 사용하며 런타임 공개 API에 STL 컨테이너를 도입하지 않는다. 안정성과 이해하기 쉬운 오류·수명 계약을 우선하고, 플랫폼·워크로드별 성능은 측정 결과로 판단한다. 일반 해시 컬렉션은 GC 추적 컨테이너가 아니며 객체 참조를 넣는 것만으로 GC root나 write barrier가 생기지 않는다.

## 찾거나 추가하기

```cpp
rocks::HashMap<rocks::int64, rocks::int32> scores(allocator);

// 두 번째 인자를 새 값으로 사용한다. 기존 값이 있으면 변경하지 않는다.
auto score = scores.FindOrAdd(rocks::int64{42}, rocks::int32{100});
if (!score) {
    return score.Error();
}
if (score.Value().outcome == rocks::InsertOutcome::Inserted) {
    // 새 항목이 만들어졌다.
}
rocks::int32* borrowedScore = score.Value().value;
(void)borrowedScore;

// 키가 없을 때만 팩토리를 호출한다. String처럼 할당기가 필요한 값에 사용한다.
rocks::HashMap<rocks::int64, rocks::String> names(allocator);
auto name = names.FindOrAddWith(rocks::int64{42}, [&]() noexcept -> rocks::Result<rocks::String> {
    rocks::String value(allocator);
    auto assigned = value.TryAssign("Rocks");
    if (!assigned) {
        return rocks::Result<rocks::String>::Failure(assigned.Error());
    }
    return rocks::Result<rocks::String>::Success(rocks::Move(value));
});
if (!name) {
    return name.Error();
}
```

`FindOrAdd(key)`는 키가 없을 때 `V{}`로 값을 만든다. 기본 생성은 `noexcept`여야 한다. `FindOrAdd(key, value)`는 전달된 값을 사용한다. 세 API는 성공 시 빌린 값 포인터와 `Inserted` 또는 `AlreadyPresent`를 반환한다. `Result` 자체의 참·거짓은 성공 여부이며, 삽입 여부는 `Value().outcome`으로 확인한다. 반환 포인터는 컨테이너를 소유하지 않으며 재해시, 해당 항목의 삭제·교체, 전체 비우기 또는 파괴 시 무효화된다.

`FindOrAddWith`의 팩토리는 인자 없는 `noexcept` 함수로 정확히 `Result<V>`를 반환한다. 기존 키에서는 실행되지 않는다. 팩토리 실패나 이후 저장소 할당 실패 시 기존 내용·용량·포인터와 키 인자는 유지된다. 팩토리의 외부 부작용은 취소되지 않는다. 팩토리 실행과 생성된 임시 값의 소멸 중 같은 Map의 구조 변경은 차단된다. 팩토리는 입력 키나 기존 원소의 내용을 직접 바꾸지 않아야 하며, 간접 참조의 수명은 호출자가 책임진다. 팩토리 중 중첩 조회는 가능하다. 삽입 키는 정확히 `K` 타입이어야 하며, 신규 키를 저장소 원소의 직접 별칭으로 전달하지 않는다.

## 조건에 맞는 원소 삭제

```cpp
auto removed = scores.EraseIf([](const rocks::int64& key, const rocks::int32& value) noexcept {
    return key < 0 || value <= 0;
});
if (!removed) {
    return removed.Error();
}
rocks::usize removedCount = removed.Value();
(void)removedCount;
```

`HashMap::EraseIf`는 읽기 전용 키·값을, `HashSet::EraseIf`는 읽기 전용 키를 전달한다. 조건 함수는 `noexcept`이고 정확히 `bool`을 반환해야 한다. `true`인 현재 원소만 제거하고, 성공 시 실제 삭제 수를 반환한다. 삭제된 원소의 빌린 포인터는 즉시 무효화된다. 삭제되지 않은 원소는 이동하지 않으며 삭제 표시가 쌓이면 이후 삽입 시 재해시가 발생할 수 있다.

조건 함수에서 같은 컨테이너를 조회하거나 중첩 순회할 수 있으나 구조 변경은 할 수 없다. 다른 순회 중 `EraseIf`를 호출하면 `Busy`다. 해시·동등 비교, 원소의 생성·이동·소멸 및 할당기 콜백 중 동일 Map에 접근하는 것은 별도의 금지 사항이다. 이 금지 사항을 위반한 모든 읽기 재진입이 자동 감지된다고 가정해서는 안 된다.

## 집합 필터와 합집합

```cpp
rocks::HashSet<rocks::int32> active(allocator);
rocks::HashSet<rocks::int32> permitted(allocator);
// 예제에서는 두 집합을 미리 채웠다고 가정한다.

// active에 있으면서 permitted에서도 찾을 수 있는 원소만 남긴다.
auto removed = active.IntersectWith(permitted);
if (!removed) {
    return removed.Error();
}

// 결과 해시/동등 정책과 복제 함수를 명시한다. 두 입력은 소유권을 유지한다.
auto combined = active.TryUnion(
    permitted, allocator, rocks::Hash<rocks::int32>{}, rocks::Equal<rocks::int32>{},
    [](const rocks::int32& key) noexcept -> rocks::Result<rocks::int32> {
        return rocks::Result<rocks::int32>::Success(key);
    });
if (!combined) {
    return combined.Error();
}
// combined.Value()는 독립된 결과 Set이다. allocator는 결과보다 오래 살아야 한다.
```

`A.IntersectWith(B)`는 `B.Contains(a)`가 거짓인 A 원소를 제거하고, `A.DifferenceWith(B)`는 참인 A 원소를 제거한다. 두 연산은 상대 집합 B의 해시·동등 정책을 이용하는 방향성 필터다. 같은 집합과의 교집합은 그대로이고 차집합은 비워진다. 비교 동등관계가 같을 때 일반적인 수학적 교집합·차집합으로 해석할 수 있다. 서로 다른 정책 상태에서는 연산 방향에 따라 결과가 달라질 수 있다. 현재 양 입력의 `Hasher`·`KeyEqual` 템플릿 타입은 같아야 하지만 그 내부 상태는 달라도 된다.

`TryUnion`은 독립적인 새 Set을 만든다. 사용자가 전달한 결과 해시·동등 정책으로 A의 원소부터 B의 원소 순서로 중복을 판단한다. 같은 결과 키로 분류되는 여러 원소 중 어느 대표가 선택되는지는 각 입력의 비결정적 순회 순서에 영향을 받으므로 영속 데이터나 결정적 로직에 의존하지 않는다. 복제 함수는 `noexcept Result<K>(const K&)`이고 결과 정책으로 원본과 동등하며 같은 해시인 키를 반환해야 한다. 이동 전용 키도 이 복제 함수를 제공하면 사용할 수 있다. 복제 또는 할당 실패 시 임시 결과는 파괴되며 양 입력과 기존 포인터는 보존된다. 팩토리·복제 함수의 외부 부작용과 결과 정책 인자의 이동은 되돌리지 않는다.

집합 연산 동안 양 입력은 외부에서 동기화해야 하며, 비교·해시·복제·할당 및 원소 수명 콜백에서 입력의 구조를 변경해서는 안 된다. `TryUnion`은 임시 결과와 복제 키의 소멸까지 양 입력의 구조 변경을 차단한다. `IntersectWith`와 `DifferenceWith`도 처리 중 상대 집합의 구조 변경을 차단한다. 순회 중 현재 집합에 필터 연산을 실행하면 `Busy`다. 같은 컨테이너의 내부 콜백에 의한 읽기 재진입까지 이 구조 변경 가드가 모두 탐지한다고 가정하지 않는다.

## 언리얼과 비교

비교 대상은 Epic의 [TMap 안내](https://dev.epicgames.com/documentation/unreal-engine/map-containers-in-unreal-engine)와 [TSet 5.5 API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/Containers/TSet?application_version=5.5)의 공개 설명이다. 이는 사용성 검토이며 내부 구현이나 동작의 완전한 호환성을 약속하지 않는다.

| 주제 | 언리얼 문서의 설명 | Rocks 설계 방향 및 현재 확인 기준 |
|---|---|---|
| 구조 | `TMap`은 키·값 쌍, `TSet`은 원소 집합이다. 두 문서는 sparse array와 hash bucket을 설명한다. | open addressing과 상태 표시를 사용하는 배열 기반 구조를 구현했다. 바이너리·API 호환은 제공하지 않는다. |
| 키 | `TMap`은 기본적으로 고유 키이며, 별도 `TMultiMap`은 중복 키를 허용한다. | 고유 키 Map/Set을 제공한다. 다중 키 컬렉션은 후속이다. |
| 중복 추가 | `TMap::Add`는 기존 키가 있으면 값을 교체한다. | `Insert`는 중복을 보존하고 `InsertOrAssign`은 기존 값을 교체한다. 언리얼 `Add`와 이름·동작이 다르다. |
| 조회·기본 삽입 | `Find`, `FindOrAdd` 등과 반복자를 제공한다. | `Find`, `FindOrAdd`, 팩토리 기반 추가와 `ForEach`·`EraseIf`를 제공한다. 외부 반복자는 후속이다. |
| 용량 | `Reserve` 등으로 추가를 위한 여유 공간을 확보할 수 있다. | `Reserve`는 원소 수 기준이며 실패를 반환한다. 삭제 표시 정리로 예약 후에도 재해시가 일어날 수 있다. |
| 키 정책 | `KeyFuncs`로 키 추출·비교·해시를 사용자 정의할 수 있다. | 사용자 해시·동등 정책과 호환되는 이종 조회를 제공한다. 별도 키 추출과 사전 계산 해시는 후속이다. |
| 순서와 주소 | `TMap`의 순회 순서는 신뢰할 수 없으며, 원소 추가 시 재할당으로 포인터가 무효화될 수 있다. | 순회 순서·원소 주소의 안정성을 보장하지 않는 것이 기본이다. 실제 무효화 규약을 각 API에 명시한다. sparse array 방식도 안정 주소를 보장한다고 추론하지 않는다. |
| 복사·소유 | `TMap`은 값 타입으로 복사·이동을 지원한다고 설명한다. | 소유 컨테이너는 move-only다. `TryUnion`은 호출자가 제공한 복제 함수로 독립 결과를 만든다. 범용 컨테이너 복제는 후속이다. |

언리얼의 sparse array는 삭제로 남은 빈 위치를 재사용할 수 있지만 전체 저장소의 재할당 가능성을 없애지는 않는다. Rocks의 open addressing은 충돌 탐사와 삭제 표시 관리가 중요하고, 재해시 시 살아 있는 원소를 다른 위치로 옮길 수 있다. 구현별 메모리 사용·캐시 효율·삭제 비용·성능 우위는 실제 측정 전에는 단정하지 않는다. 주소 안정성이 필요한 사용처는 별도의 안정 저장소와 핸들 또는 소유 객체를 검토한다.

## 자료구조와 기본 계약

- Map은 고유 키와 값을 소유하고, Set은 고유 원소를 소유한다. Set이 Map을 재사용하더라도 의미 없는 값이나 잘못된 공개 수정 경로가 노출되지 않아야 한다.
- 비어 있음·사용 중·삭제됨을 구분한다. 삭제된 위치를 넘어서 기존 키를 끝까지 탐색해야 하며, 비어 있는 위치 또는 탐사 상한에서 종료한다. 삭제 자리를 재사용할 때도 뒤쪽 중복 키 확인을 먼저 완료한다.
- 키가 동등하면 해시도 같아야 한다. 보관 중 키와 해시·동등 정책의 의미는 바뀌면 안 된다. 외부에는 키를 수정할 수 없는 형태로 노출한다. 정책 함수의 `noexcept`, 결정성 및 재진입 제한은 실제 템플릿 계약에 명시한다.
- 저장소 할당·해제는 명시적으로 받은 `Allocator`를 사용하고 할당기가 컨테이너와 모든 사용보다 오래 살아야 한다. 할당 실패를 성공한 빈 슬롯으로 취급하지 않는다. capacity와 byte 계산, 정렬 및 증가 과정의 정수 overflow를 검사한다.
- 원소는 적절한 정렬의 raw storage에 생성하고 생존 중인 원소만 소멸한다. 임의 타입에 byte 복사만으로 이동을 가정하지 않는다. 이동·소멸 등의 허용 조건은 실제 지원 타입에 맞추어 컴파일 시 검사한다.
- 같은 컨테이너의 동시 접근은 호출자가 동기화한다. `const ForEach`도 내부 순회 상태를 변경하므로 읽기 전용 순회끼리도 동기화가 필요하다. 해시·비교·생성·소멸·할당 콜백이 같은 컨테이너를 재진입 변경하는 것은 기본적으로 금지한다. 락 기반 공유 또는 동시 컨테이너는 별도 설계다.
- 해시와 순회 순서를 파일, 네트워크, 영속 ID 또는 결정적 게임 로직의 기준으로 사용하지 않는다. 결정적 출력은 별도 키 정렬과 명시적 인코딩으로 만든다. 악의적 입력의 충돌 공격이나 상수 시간 조회도 보장하지 않는다.

## 사용성 목표와 우선순위

| 우선순위 | 기능과 제안된 의미 | 완료 기준 |
|---|---|---|
| P0 | 생성·이동·소멸, 크기/빈 여부, 동등 키 조회, 존재 확인, 중복 보존 삽입, 삭제, 비우기 | 고유 키와 충돌 탐사, 원소 수명·포인터 무효화, 다른 할당기 간 이동, 빈 상태와 재사용 테스트. 구현·헤더·테스트가 일치해야 함. |
| P0 | 실패 가능한 사전 확보와 확장 | 오버플로와 각 할당 실패에서 원래 내용·용량·기존 참조 유지 여부를 계약에 맞게 확인. 성공 재할당에서 무효화 명시. |
| P0 | 오류가 없는 조건문용 `Contains`와 조회 포인터 등 | 조회 실패와 정상 미존재를 구별해야 하는 경우 별도 결과를 사용한다. `Result<bool>`의 bool 변환을 실제 값으로 오인하는 API는 피한다. |
| P0 | 중복 보존 삽입 결과 | 성공/실패와 새 삽입/기존 발견을 명시적으로 구분한다. 중복 발견 또는 할당 실패 시 인자와 기존 값을 어떤 범위로 보존하는지 문서화하고 검증한다. |
| P1 | 기존 키 값을 명시적으로 교체하는 삽입, 기본값 또는 생성자로 찾거나 추가 | `TMap::Add`의 교체 의미와 혼동하지 않는 이름·결과를 사용한다. 이동 전용 값, 생성 실패 처리, 기존 값과 신규 인자의 alias 및 실패 원자성을 검증한다. |
| P1 | const key·가변 value를 제공하는 순회, const 순회, 안전한 삭제 패턴 | 빈 슬롯과 삭제 슬롯을 건너뛰고 누락/중복 없는 독립 oracle, mutation 시 반복자 무효화, const 위반 컴파일 검사를 갖춘다. 순회 중 삭제는 지원 여부를 별도로 정한다. |
| P1 | 사용자 정의 hash/equality, 문자열·Name·Guid 등 자주 쓰는 키 | 동등성과 해시 일치, 충돌, 잘못된 정책의 계약, UTF-8 바이트/대소문자/정규화 정책, 보관 수명을 테스트한다. 모든 타입의 기본 지원을 가정하지 않는다. |
| P2 | 소유 문자열 키의 이종 조회와 명시적 복제 | 임시 문자열 할당 없이 찾는 경우 양쪽 해시·비교의 일치를 검증한다. 복제 실패 시 부분 객체와 할당을 정리하고 원본을 보존한다. |
| P2 | 범위 삽입, Set 합집합·교집합·차집합, bulk 예약·축소 | 부분 성공 또는 전체 롤백을 기능별로 정하고 중복·자기 입력·할당 실패·원소 수명을 검사한다. |
| P3 | 정책 기반 키 추출, 사전 계산 hash, 다중 키, 소형 저장소, 안정 핸들 변형 | 별도 사용 사례와 계약을 먼저 수립한다. 호출자 제공 hash의 일치 검증과 잘못된 사용의 진단, ABI 및 다른 컨테이너와의 차이를 평가한다. |

`Try`는 실패를 호출자가 처리한다는 의미로 사용한다. 제공되는 삽입 결과와 콜백 계약은 위 사용법과 공개 헤더를 따른다. 작업 실패 시 무엇이 유지되는지도 API마다 확인한다.

## 수명·참조·문자열

일반적인 조회 포인터와 반복자는 소유권을 주지 않는다. 컨테이너 파괴·이동 대입·삭제·비우기와 재해시에 어떤 참조가 무효화되는지 각 구현에서 보수적으로 정하고 테스트한다. 비재할당 삽입도 구현이 원소를 이동시키는 방식이라면 기존 참조가 무효화될 수 있으므로, 그 안정성을 검증하기 전에는 가정하지 않는다. 호출자는 조회 포인터가 살아 있는 동안 충돌을 일으키는 변경을 피한다.

성장 삽입에서 기존 원소를 인자로 넘기는 경우, 이전 저장소가 해제되거나 이동되기 전에 필요한 인자 사용을 마쳐야 한다. 특히 키와 값 인자가 서로 또는 다른 원소를 참조하거나 rvalue로 이동되는 경우는 단순 포인터 안정화만으로 안전해지지 않는다. 지원 가능한 조합과 금지 조합을 공개 계약으로 제한하고, 지원하는 경로는 확장 성공·중복·할당 실패 각각에 대한 회귀 테스트를 둔다. 허용하지 않는 사용에 대해 감지하지 못하는 경우 이를 런타임에 완벽히 진단한다고 주장하지 않는다.

`StringView`를 키로 저장하면 바이트를 소유하지 않는다. 원본이 파괴·재할당되거나 바이트 내용이 바뀌면 조회가 깨지거나 수명 위반이 될 수 있다. 내장 NUL을 포함하는 문자열은 길이를 함께 비교한다. 소유가 필요한 키는 `String` 등 소유 타입을 사용하고 해당 hash/equality와 명시적 복제의 실제 제공 여부를 확인한다. `Name`의 도메인·pool 수명에 관한 별도 규약도 따른다.

## 검증과 관측

1. **독립 정확성**: 단순 참조 모델과 고정 seed의 무작위 삽입·조회·삭제 시퀀스를 비교한다. 모든 키가 같은 hash인 경우, 배열 끝을 넘어 순환하는 탐사, 삭제된 슬롯 뒤의 중복 키, 전부 삭제한 뒤 재사용, 확장 임계점을 포함한다.
2. **자원과 실패**: 할당 실패 지점을 순서대로 주입한다. 실패 전후의 값·수·용량·주소 계약과 생성/이동/소멸 회계, allocator의 live allocation 및 live bytes가 0인지 확인한다. 정렬이 큰 타입, 이동 전용 타입, 최대 크기 overflow를 포함한다.
3. **사용자 실수와 계약**: 키 수정 불가, 지원하는 자기 참조 입력, const 조회, 빈 컨테이너, 경계 입력, moved-from 재사용, 해시·동등 불변식을 검사한다. 잘못된 계약은 조용히 다른 결과로 바꾸지 않는다.
4. **플랫폼과 도구**: Windows·macOS의 Debug 및 Release에서 경고를 오류로 처리하고 공개 헤더 단독 컴파일, 포맷, 단위/하네스 및 가능한 메모리 검사(AddressSanitizer)를 실행한다. 수행하지 못한 플랫폼이나 검사 항목은 별도로 표기한다.
5. **성능은 별도 게이트**: 실제 엔진 키 분포와 크기별 탐사 길이, 로드율, 삭제 누적, 재해시 시간·메모리, 할당 횟수를 재현 가능한 측정으로 기록한다. 한정된 합성 벤치마크 결과만으로 언리얼 대비 우위를 주장하지 않는다. 장시간·악의적 충돌 부하와 프레임 지연은 별도 평가한다.

구현이 완성되었다고 판단하려면 기능 목록별 공개 계약, 성공·실패·경계 테스트, 위 플랫폼별 실제 결과와 미지원 목록이 함께 정리되어야 한다. 새로운 컬렉션 한 종류의 완성은 전체 Core나 Foundation 완료를 뜻하지 않는다.

## 내부 콜백 재진입 진단의 후속 과제

현재 원소 생성·이동·소멸, 해시·동등 비교 및 할당기 콜백은 같은 컬렉션에 읽기를 포함해 재진입하면 안 된다. 순회·팩토리·집합 연산에 적용한 구조 변경 가드는 이 금지된 모든 읽기 경로를 자동으로 진단하지 않는다. 특히 원소의 수명이 끝나는 도중에 기존 포인터를 읽는 것은 지원하지 않는다.

완전한 진단은 별도의 콜백 상태와 공개 API 진입 검사, 정책 객체의 이동·소멸을 포함한 수명 관리가 필요하다. 정상적인 순회 predicate와 팩토리의 중첩 읽기는 계속 허용해야 한다. 재진입 음성 대조, 실패 후 상태 복원, 두 컬렉션 간 접근, 플랫폼별 메모리 검사와 성능 측정을 거쳐 별도 작업으로 구현한다. 수명 종료 후 포인터 접근은 C++ 규약 위반이므로 이 진단으로 보장할 수 없다.
