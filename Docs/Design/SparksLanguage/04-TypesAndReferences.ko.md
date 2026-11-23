## 4. 타입·참조·값

### 4.1 값과 참조

스크립트가 직접 다루는 데이터는 값이다. 참조는 엔진이 수명을 관리하는 객체(Actor·Component·Service·WidgetController)와 에셋뿐이며, 스크립트는 GC로만 회수되는 객체를 생성하지 않는다.

그림 1. 값 영역과 엔진 객체

- 값 영역(VM 소유·참조계수·COW): handler 실행 frame이 Scalar(`s0 int`, `s1 float`)·Ref(`r0 this`, `r1 Door`)·Buf(`b0 Array<Enemy>`) bank를 가진다. Ref는 handle+generation이며 GC root를 만들지 않는다.
- 참조계수 소유: Buf `b0`와 엔진 객체 Door의 `Targets: Array<Enemy>` 필드가 같은 COW 버퍼(rc=2)를 공유한다. 값은 참조계수 버퍼이며 closure가 없으므로 순환이 형성되지 않고 참조계수 0에서 회수된다.
- 약한 참조: Ref `r1`과 버퍼 원소 `[0] Enemy A`·`[1] Enemy B`는 엔진 객체를 약한 handle로 가리킨다. 엔진 객체 참조는 모두 약한 handle이며, 엔진 객체의 수명은 World·Actor의 소유 관계가 정한다.
- 에셋: 에셋 참조는 lease를 공유하는 값이며 보관하는 동안 상주를 유지한다(§7).

본문 규칙은 다음과 같다.

- `string`·`Array`·`Map`·`Set`은 참조계수 버퍼이며 공유 중인 버퍼를 변경할 때 먼저 복사한다.[R13] closure가 없으므로 값 사이 순환이 없고 참조계수만으로 회수된다. VM은 스크립트 상태에 대해 GC root를 제공하지 않는다(전체 계획 §23.2 개정).
- 소유 component 필드(`let PickupRange = SphereCollider(Radius: 200.0)`)도 handle이다. 엔진 쪽 표현은 Prefab 3.2절의 소유 subobject 필드(`ObjectRef<…>`, SubobjectGuid)이며 수명은 소유 Actor가 관리한다. 스크립트 필드는 두 번째 수명 관리자가 아니고 VM에는 약한 handle로 보인다.
- 참조계수는 Game lane 전용이다. 다른 lane으로 값을 전달할 때는 버퍼를 고정(freeze)하고 참조계수를 1 증가시켜 넘기며, 해제는 Game lane으로 되돌려 처리한다(§9.4).

### 4.2 기본 타입

| 분류 | 타입 | 규칙 |
|---|---|---|
| 논리·숫자 | `bool`, `int8`–`int64`, `uint8`–`uint64`, `float32`, `float64`, 기본형 `int`(=`int64`)·`float`(=`float64`) | 규칙은 §3.3(SD56·SD57). 엔진 필드·인자는 C++ 타입과 같은 Sparks 타입으로 보이므로(C++ `int32` → `int32`, `float` → `float32`, `double` → `float64`) 경계에서 따로 변환하지 않는다. 수학 타입(`vec3` 등)의 성분은 `float64`이며 엔진의 float32 벡터와 주고받을 때 최근접 짝수로 반올림하고 범위 밖은 `NonFinite`다 |
| 수학 | `vec2`–`vec4`, `quat`, `mat4`, `Rotator`, `WorldPosition`, `Transform`, `Color` | Core 수학 타입의 바인딩(연산과 메서드는 부록 B). `WorldPosition`은 엔진에 아직 없는 자리표시 타입이며(연동 요구 SE-G13) 이름만 예약하고 규격은 엔진 타입이 생길 때 정한다. 스크립트 안의 성분은 `float`(64비트)이고 엔진에 전달할 때 위 규칙으로 변환한다. 단위·축은 엔진 규약(UE 5.8과 같은 왼손 좌표계, +X 전방·+Y 오른쪽·+Z 위, yaw 양수는 오른쪽 회전, 길이 cm·질량 kg·시간 초, `Rotator` 도, 삼각함수·quaternion 축-각·각속도 라디안)을 따른다(전체 계획 §2.2, Math 구현 문서의 Rotator 규약). 각도 단위가 둘이므로 각도를 받는 native 함수·필드는 UE처럼 이름이나 타입으로 단위를 드러내고(`Rotator` 필드는 도, `…InRadians`·`…InDegrees`), 언어 서버가 hover·자동완성에 단위를 표시한다(SD18) |
| 문자 | `string`, `Text` | 화면 표시 API는 `Text`만 받는다. `Text`는 `Loc(...)`로 생성 |
| 엔진 데이터 | `Tag`, `TagSet`, `InputAction`, `Soft<T>`, `Row<T>` | §3.5 리터럴. `Soft<T>`·`Row<T>`에는 빈 값이 없으며 선택 값은 `Soft<T>?`다(SD19) |
| 컨테이너 | `Array<T>`, `Map<K,V>`, `Set<T>` | 삽입 순서로 순회. 번호·키 읽기는 `T?`. 범위 밖 쓰기는 fault. Map의 `m[k] = v`는 삽입 또는 교체이고, 복합 대입과 경로 쓰기는 키가 없으면 fault. 무공변(`Array<Derived>`는 `Array<Base>`가 아니다). 타입 인자에는 `?`를 쓸 수 없다. C++ 쪽은 Core `OrderedMap`·`OrderedSet`(삽입 순서 보존)이고 키는 `==` 가능한 단일 값(정수·bool·enum·문자열·Name·Guid·Soft·클래스 참조 — 실수·객체 키는 저장할 수 없다)이다(RX 7절). 따라서 객체 키 Map·객체 원소 Set은 VM 전용(비영속·비복제)이며 `@edit`·`@save`·`@net` 필드에 두면 컴파일 오류다 |
| native 값 struct | binding된 C++ 값 struct(커브 `TangentCurve`·`PlainCurve`·계단 커브, `Hit` 등) | 값 의미(COW), `==` 불가, 저장 가능 여부는 구성 필드로 판정. 메서드는 binding이 노출한다(예: `Falloff.Eval(t): float` — CurveData 14절, float32 결과는 float64로 정확히 올라가고 인자는 위 변환 규칙) |
| 합성 | `T?`, `Result<T>` | 조회 실패는 `T?`, 작업 실패는 `Result`. `r.Value: T?`, `r.Error: Error?`이며 `Result<void>`에는 `Value`가 없다. `?`는 중첩되지 않는다 |
| 클래스 참조 (full) | `Class<T>` | TypeId와 기반 타입 제한. `Create(...)`. C++ 쪽은 `ClassRef<Base>`(리플렉션 B9 — ReflectionExtensions 5.2절) |

### 4.3 객체 참조와 "없음"

| 위치 | 타입 | 유효 범위 |
|---|---|---|
| `this` | `T` | 실행 중 항상 유효. 재개마다 generation을 확인하고 사라졌으면 실행을 취소 |
| `Owner`(component의 속성) | `Actor` | 실행 중 항상 유효. Destroy hook에서는 읽기 전용. 엔진 `Component::GetOwner()`의 binding |
| `Widget`(WidgetController의 속성) | widget 타입 | 실행 중 항상 유효 |
| 소유 component 필드 | `T` | 소유 Actor와 같은 수명 |
| `?` 없는 `@edit` 객체 필드 | `T` | 같은 prefab 안의 참조만 허용. non-null 검사는 Sparks 쿡·spawn 검증이 하며 리플렉션 Required(약한 참조에는 RMC2202로 금지)를 쓰지 않는다. spawn 시 해석에 실패하면 spawn 실패. 같은 파괴 묶음 밖에서 대상이 제거되면 참조하는 component를 격리(§8.3) |
| 그 밖의 객체 필드 | `T?` | 파괴되면 null. 다른 셀의 Actor는 셀이 상주할 때 자동 해석(엔진 `WeakObjectRef` 필드는 같은 저장 단위 안만 보존하므로 셀 사이 참조는 World의 영속 entity 참조가 맡는다 — RX 5.1절, Prefab D12. 영속 참조가 들어오기 전에는 셀 사이 참조 저장을 거부한다) |
| 컨테이너 원소 | 순회 변수 `T`, 번호·키 읽기 `T?` | `Array`는 파괴된 원소의 자리를 유지하고 null로 읽으며 순회는 살아 있는 원소만 제공한다. 객체 원소 Array에는 `Count`가 없고 `SlotCount`·`AliveCount()`로 뜻을 드러낸다(SD40). `Set` 원소와 `Map` 키는 파괴가 적용된 뒤 처음 접근할 때 제거한다(SD14) |
| 지역 이름(인자·지역 변수·`if (let)` binding·반복 변수) | `T` 또는 `T?` | `await`를 지난 뒤 읽으면 `T?`(아래) |

- **await 이후:** 어느 경로로든 `await`를 지난 뒤 읽는 지역 이름의 객체 타입 `T`는 그 읽기 지점에서 `T?`다. 반복문의 back-edge와 분기 합류도 포함하며, 그 변수에 다시 대입하면 `T`로 돌아간다. 이 규칙은 엔진 객체(Actor·Component·Service·WidgetController) 타입의 지역 이름에만 적용하며 에셋 값·유지 자원·로드 요청 handle을 담은 지역 이름은 해당하지 않는다. `this`, 문맥 이름, `Owner`·`Widget`, 소유 component 필드, `?` 없는 `@edit` 필드는 지역 이름이 아니므로 바뀌지 않는다.
- **null 검사는 하나다.** `if (let ...)`, `?.`, `??`, `== null`은 모두 "null이거나 파괴됨"을 같은 판정으로 처리한다. 타입을 좁히는 형태는 `if (let)`과 `?? Exit`뿐이며 `== null` 비교는 타입을 바꾸지 않는다. `?.`·`??`·`if (let)`의 대상이 `?` 타입이 아니면 오류다. `if (let x = e)`의 `e`가 `bool?`이면 오류이며, 참인지 볼 때는 `e ?? false`, 있는지 볼 때는 `e != null`을 쓴다.
- **`?.`와 `??`:** 수신자가 null이면 `?.` 바로 뒤의 이름과 그 호출 인자를 평가하지 않으며 결과는 `R?`(호출 결과가 `void`면 `void`)다. 단락은 `?.` 하나에만 적용되고 이어지는 `.`는 `R?` 수신자 규칙을 따르므로 `a?.B?.C`로 쓴다. `??`의 좌변은 `T?`, 우변은 `T`(결과 `T`)·`T?`(결과 `T?`)·`Exit`이며 기반 타입으로 합치지 않는다.
- **파괴는 스크립트 단계 끝에 World에 적용된다(두 시점, SD25).** 스크립트 단계가 열려 있는 동안 Game lane에서 요청된 파괴·활성 전환(스크립트, 스크립트가 호출한 native API, 전달 순회(§5.4)가 호출한 C++ handler)은 스크립트 bridge의 deferred 큐에 들어가고, 그 단계의 이벤트 전달이 끝난 뒤 요청 순서대로 World API(`World::DestroyActor`·`DestroyComponent`·`SetEnabled`)를 호출한다. 이것이 **적용 시점**이다. 적용된 객체는 엔진의 PendingDestroy 상태가 되어 Tick에서 빠지고, 실제 Disable·Destroy hook과 Detach는 World의 **프레임 끝 정리**에서 실행된다(ER 7.4). 같은 단계 안에서 확인한 객체는 단계 끝까지 유효하며 순회·`if (let)`에도 그대로 남는다(SD3). 적용 시점 이후 스크립트는 그 객체를 null로 읽는다. 객체 원소 Array의 `SlotCount`는 파괴된 자리를 포함하고 `AliveCount()`는 살아 있는 원소만 센다(SD40). 생성(`World.Spawn`)은 즉시 수행된다(§5.1).
- **파괴 묶음:** 한 번의 적용에서 같은 Actor graph(Actor와 그 component·부착 자식)의 파괴는 한 묶음으로 처리한다. 프레임 끝 정리는 묶음 단위로 Disable 전부(graph 역순) → Destroy 전부(graph 역순) → Detach 전부 순서이며 엔진 정리 순서(Component OnDisable → Actor OnDisable → Component OnDestroy → Actor OnDestroy → OnUnregister)와 같다. 같은 묶음 안의 `?` 없는 `@edit` 참조 대상 제거는 격리 사유가 아니다.
- **컨테이너 정리는 값 변경이 아니다.** 파괴로 인한 null 읽기와 Set·Map 항목 제거는 Changed·복제·저장을 일으키지 않으며 `let`에도 적용된다. Array의 죽은 자리는 대입·제거로만 없어지고, 증가는 장시간 soak Gate(§9.3 G2)가 검출한다.
- downcast는 `as?`만 제공한다. class 타입의 하향 변환과 interface 조회(full)에만 쓰고 입력이 null이면 결과도 null이다. 강제 unwrap은 없다.[R24]

### 4.4 대입 규칙

대입:

~~~text
Hinge.RelativeLocation.Z += 0.1;         // 쓰기 가능한 native 속성: 읽기 → 수정 → 쓰기
Points[i].X = 0.0;                        // 배열 원소에 기록
GetTransform().Location.Z = 1.0;          // 오류: 함수 결과는 대입 대상이 아니다
var p = Hinge.RelativeLocation;
p.Z = 5.0;                                // 오류: 바꾼 사본을 어느 경로에서도 읽지 않는다
for (h in Hits) { h.Damage = 0; }         // 오류: 반복 변수는 바꿀 수 없다 → Hits[i].Damage
other.Current = 0;                        // 오류(other: Health): 다른 객체의 Sparks 필드는 읽기만 한다
~~~

- 대입은 지역 변수, 자기 instance의 필드, 원소 경로, 쓰기 가능한 native 속성 경로에만 할 수 있다. 경로 중간의 값 타입은 원본에 기록된다. 반복 변수와 함수 인자는 바꿀 수 없다.
- 사본 수정 오류는 미사용 규칙(§3.3)의 한 경우다. 바꾼 `p`를 이후에 읽거나 `Hinge.RelativeLocation = p;`로 되돌려 쓰면 오류가 아니다.
- `let`의 규칙은 §3.2에 있다. C++ `ReadOnly` 필드는 엔진 쪽에서 바뀔 수 있는 읽기 전용 속성으로 표시한다.

### 4.5 생성과 기본값 순서

- struct와 component는 필드 이름으로 생성한다(`HitInfo(Point: p)`). 지정하지 않은 필드는 기본값을 사용하며 기본값이 없는 필드(§3.2)는 반드시 지정한다.
- component는 `let` 필드의 초기값으로만 생성한다(실행 중 추가는 full). 편집기에서 배치한 Actor에 ScriptableComponent를 붙이는 것은 스크립트의 생성이 아니라 레벨·배치 데이터 편집이며 core에서도 된다: 직접 배치 Actor는 그 `components`에 추가해 레벨에 저장하고, prefab 배치는 instance override의 노드 추가로 저장한다(Prefab PF4 선행, [LevelScript](../LevelScript.ko.md) LD15). 필드 초기값은 상수식과 component 생성만 허용하고 클래스 기본값 레코드에 반영한다 — 레코드는 가장 가까운 C++ base의 생성자 결과 위에 컴파일된 초기값을 덮어 합성한다(RCM '클래스 기본값 결정'). component 생성 초기값은 클래스 원형의 default subobject(Prefab PF1)가 선행 조건이며 그때까지 미지원이다. 상수식은 리터럴, 검증 리터럴, enum 값, const 이름, 이들의 산술·비교, 수학 타입·struct 생성, 배열·Map 리터럴, `Math.Radians`·`Math.Degrees`이며 그 밖의 함수 호출과 필드 참조는 포함하지 않는다. 컴파일 시 overflow는 오류다. C++ 생성자 안에서 스크립트 코드를 실행하지 않는다.
- 기본값 순서는 전체 계획 §5.3과 Prefab 3.1절을 따른다: native → Sparks 필드 초기값(이 둘의 결과가 클래스 기본값 레코드다) → subobject 기본값 레코드 → prefab·variant → instance override → spawn parameter. 코드의 기본값 변경은 override하지 않은 instance에 반영하고 override한 instance를 편집기에 표시한다.
- 소유 component 필드에도 안정 ID(SubobjectGuid)를 부여하므로 이름을 바꿔도 prefab override가 유지된다(전체 계획 §5.3).
