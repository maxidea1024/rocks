## 15. 저장·데이터·편집 연동

`Props/Chest.spark`:

~~~text
import Items.ItemData;

@tableRow(Asset("/Game/Data/LootTable"))
struct LootRow {
    var Item: Soft<ItemData>;
    var Weight: float;
}

class Chest : ScriptableComponent {
    @edit var Lid: SceneComponent;                 // 같은 prefab 안의 필수 참조
    @edit var Loot: Row<LootRow> = Row("Chest.Common");
    @edit(Min: 0, Max: 10) var Uses: int = 3;
    @save @formerly("Opened") var IsOpened: bool;

    on Enable() { ApplyLid(); }                        // 로드된 상태 반영(활성화마다)
    on IsOpened.Changed() { ApplyLid(); }

    fn ApplyLid() {
        if (IsOpened) {
            Lid.RelativeRotation.Pitch = 110.0;            // 도
        } else {
            Lid.RelativeRotation.Pitch = 0.0;
        }
    }
}
~~~

- **저장 가능 타입:** `bool`·모든 숫자 타입·수학 타입·`string`·enum·`Tag`·`TagSet`·`Soft<T>`·`Row<T>`, 이들의 `T?`, 그리고 이들로만 구성된 struct·컨테이너. 엔진 객체(자기 component 포함)·로드한 에셋 값·`Text`·`Result`는 저장할 수 없다(전체 계획 §7.2). C++ 대응(리플렉션 B9 — ReflectionExtensions 19절): `Tag` → Name, `TagSet` → `OrderedSet<Name>`, `Row<T>` → Name, `T?`(단일 값·문자열·struct) → `Optional<T>`, `Tag?`·`Row<T>?`는 None Name(없음은 하나 — SD19).
- **호환성:** 매 컴파일 검사. 필드 추가는 기본값(또는 zero value), 삭제는 `retire`, 이름 변경은 `@formerly`, 무손실이 아닌 형식 변경은 migration 함수. enum은 case 이름으로 저장하므로 case 추가 위치는 자유이고 삭제는 `retire`다.
- **로드:** 무손실 변환이 가능하면 변환하고, 불가능하거나 알 수 없는 enum 값이면 기본값을 쓰고 보고한다. 클래스를 찾지 못하면 원래 필드를 보존하는 placeholder로 로드한다. 저장 파일에도 RPC와 같은 길이·값 검사를 적용한다.
- **범위 attribute:** `@edit(Min, Max)`는 편집기 입력 범위이자 실행 중 계약이다. 범위를 벗어난 값을 쓰면 `AssertFailed` fault이고, 저장 로드에서 범위를 벗어나면 가장 가까운 경계로 고치고 보고한다.
- **데이터 에셋:** `DataAsset`과 `@tableRow` struct는 실행 중 읽기 전용이다. `@edit` 배열 원소에는 ElementGuid가 없다(Prefab D4). 원소를 식별해 override해야 하는 데이터는 key 컨테이너(Map·Set) 또는 struct 안의 ID 필드로 표현한다.
- **enum 저장:** case 이름으로 저장하므로 순서 변경에 안전하다. case 삭제는 `retire`, 이름 변경은 `@formerly`다.
