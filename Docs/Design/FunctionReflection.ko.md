# 함수 리플렉션(CU2)

이 문서는 C++ 멤버 함수의 리플렉션 `ROCKS_FUNCTION`(UiToolkit CU2, UE `UFUNCTION` 대응)을 정한다: 선언 규칙과 지정자, 런타임 함수 schema, 생성 호출 frame·thunk, 범용 호출 API, 식별(FunctionId), 단계. 사용처는 Sparks binding(EI1b — [SparksBinding](SparksBinding.ko.md)), UI 바인딩 변환 함수(UiToolkit 5절), 치트 함수(CVar 14.3절 CVR8), Inspector 버튼·에디터 명령(UiToolkit 7.3 (c)), 함수 바인딩 이벤트(InputActions IA7 — rocks-d2 요구). 구조는 UE 5.8을 따르고 이름은 Rocks식이다(UE 이름은 대응표에만).

- 개정 4(2026-11-26): FR1 구현(7절) — 값 매개변수는 복사 생성 가능해야 한다(`Invoke`를 같은 frame으로 다시 부르므로 인자를 옮기지 않는다 — `String`·컨테이너·`SoftObjectRef`는 `const&`, FD4), private·protected 함수는 `ROCKS_GENERATED_BODY` 필요(FD1), 클래스 밖 정의(물려받은 annotation) 허용, 매개변수 16개·클래스당 함수 4096개 상한, descriptor schema 5 모양(FD10). 리뷰 반영 — frame 칸은 정렬된 byte 저장소(`detail::FrameSlot<T>`)라 표준 배치가 아닌 struct 인자에서도 frame이 표준 배치(FD7), frame 이름은 길이 접두 인코딩, 연산자·ASCII 아닌 함수·매개변수 이름 거부, `noexcept(true)`·끝 반환형·void typedef 반환·반환 추론(`auto` — Clang이 정한 형식) 허용과 조건부 `noexcept` 거부(FD1), `StringView` 반환 거부·`ObjectHandle`은 최상위 칸만(FD4·FD5), descriptor reader의 도구 이름 겹침 거부.
- 개정 3(2026-11-26): FR1 착수 결정 — 리플렉션 함수 이름에 C++ 오버로드 금지(정규 형식 탐침이 `&Owner::Func`로 매개변수 형식을 푼다, FD2), struct·컨테이너 인자는 같은 C++ 형식의 값 복사(경로 편집은 요구 시, FD9), 매개변수의 enum·struct 형식은 레지스트리 의존 연결 없이 생성 setter가 C++ 형식으로 검사(FD10).
- 개정 2(2026-11-26): 교차 검증 반영 — 대상 해석(const는 읽기, 그 밖은 쓰기 — `Busy`)과 Registry 오류 목록(FD8), frame에 담을 수 없는 강한 참조 형식 거부(FD4·FD5), 매개변수마다 합성 `FieldSchema`·값 ops로 필드 쓰기 도구 재사용(FD9), 함수 이름 공간을 필드와 분리·리플렉션하지 않은 C++ 오버로드 허용(FD2), `FunctionCall`이 `TypeLease`를 쥠(FD9), ID 명세·descriptor schema 5(FD3·FD10), 접근 수준 제한 없음(FD1), 치트 `Help`는 실행 schema(FD12), FR3 제약과 `invokeBase` 예약(FD13), 매개변수 flag·원소 상한(3절), EditorOnly 두 변형 비교·바인딩 쿠킹 거부(FD14·FR4), 자동 Pure 없음(FD6), `noexcept` 필수.
- 개정 1(2026-11-26): 첫 판 — 결정 FD1–FD14, 단계 FR1–FR4. rocks-d2(World·Gameplay·InputActions) 요구 7가지 반영(4절).

## 1. 단계

| 단계 | 내용 | 선행 |
|---|---|---|
| FR1 함수 schema·호출 | `ROCKS_FUNCTION` 해석(멤버·static, const), 매개변수·반환 schema, FunctionId, 생성 frame·thunk, `TypeSchema::functions` 등록·조회, 범용 호출 `FunctionCall`, 서명 비교 | 없음 |
| FR2 표시 메타 | 함수 `DisplayName`·`Category`·`Help`(인자 포함) | UiToolkit CU1의 편집 메타데이터 블록(UT0) |
| FR3 스크립트 재정의 | `ScriptOverridable`과 dispatch(UiToolkit 7.3 (f) — 생성 대리 클래스) | FR1, Sparks host(EI2·EI3) |
| FR4 함수 바인딩 | (객체 handle, FunctionId) 바인딩과 바인딩 시점 서명 검사 — 이벤트(CU3)·InputActions 바인딩. 쿠킹은 EditorOnly 함수를 가리키는 바인딩을 거부한다 | FR1, CU3 |

Sparks EI1b(schema `functions`)는 FR1 뒤에 Sparks 지정자(lane·효과·자원 정책 — SL 10.2절)를 더해 한다. 노출 이름의 겹침(필드·계산 속성·함수가 한 Sparks 이름 공간 — SL 10.3절)·`pure` 판정·generic 노출(`Find<T: Component>` — FR1은 함수 템플릿을 받지 않으므로 `ClassRef` 인자와 반환 타입 규칙으로)은 EI1b가 정한다. 치트 지정자(`Cheat`·`Authority`)는 CVR8이 더한다(지정자 이름 공간만 여기서 정한다 — FD12).

## 2. 결정

- **FD1 대상:** `ROCKS_CLASS` 클래스의 멤버 함수(인스턴스·`static`)만 받는다. struct·자유 함수는 받지 않는다(UE도 `UFUNCTION`은 `UCLASS` 안 — 라이브러리 함수는 클래스의 `static` 함수, UE `UBlueprintFunctionLibrary`). 선언 바로 앞에 `ROCKS_FUNCTION(...)`을 둔다(`[[clang::annotate]]` — `ROCKS_PROPERTY`와 같은 자리 규칙). 접근 수준(public·protected·private)은 제한하지 않는다: 생성 thunk는 `ROCKS_GENERATED_BODY`가 선언한 friend(`GeneratedType<T>`)로 부른다(필드와 같다 — private·protected 함수는 `ROCKS_GENERATED_BODY`가 있어야 한다, RMC2701). 헤더 안의 클래스 밖 정의(`inline void Door::Open() noexcept {}`)는 선언의 annotation을 물려받은 재선언이라 자리 검사 대상이 아니다. 누가 부를 수 있는지는 사용처의 지정자가 정한다(Sparks 노출·치트 등). 함수 템플릿·`= delete`·rvalue·참조 한정자 멤버, 연산자·변환 함수(이름이 ASCII 식별자가 아니다 — 개정 4, ID 명세·thunk 이름에 쓸 수 없다. `Equals` 같은 이름 함수로), 조건부 `noexcept`(`noexcept(true)`·끝 반환형·반환 추론 `auto`는 받는다), 부모의 리플렉션 함수를 파생이 다시 `ROCKS_FUNCTION`으로 선언하는 것(UE `UhtFunction.cs:862-873` — 재정의는 매크로 없이 C++로만)은 오류다. 함수는 `noexcept`여야 한다(다른 생성 ops와 같이 static_assert).
- **FD2 이름:** 리플렉션 함수 이름(도구 이름 `Name = "…"`, 없으면 C++ 이름)은 한 클래스와 그 기반 체인의 **리플렉션 함수 사이에서** 대소문자 무시로 유일하다(UE `UhtFunction.cs:1015-1020`은 자기 클래스만 보지만 Rocks는 상속 조회가 이름으로 하므로 체인까지). 필드와는 이름 공간이 다르다 — Rocks getter 관례(`Mobility()` 함수와 `mobility` 필드)가 그대로 리플렉션되게(UE shadowing 오류 `UhtProperty.cs:2310-2357`와 다른 점). 리플렉션 함수의 C++ 이름에는 오버로드가 없어야 한다(개정 3 — 메타 컴파일러가 `&Owner::Func`로 매개변수·반환 형식을 정규 철자로 풀기 때문이다, UE는 thunk가 `execFoo`라 오버로드와 무관하다). 오버로드된 함수(`AttachTo(parent)`·`AttachTo(parent, rules, …)`)는 다른 이름의 함수로 리플렉션한다 — 요구가 쌓이면 탐침을 서명 지정으로 넓힌다. 매개변수 이름은 C++ 이름(선언에 이름이 없으면 오류)이고 함수 안에서 유일하다.
- **FD3 FunctionId:** `.rocksids.json`의 `function` kind 항목이다. 이름 키는 `<정규 클래스 이름>::<C++ 함수 이름>`(필드 `field` kind와 같은 모양 — owner 검사도 필드와 같다). 지금 C++ 정규 이름은 `type`·`field`만 받으므로 FR1이 `StableIdManifest` 검증(`function` kind의 C++ 이름 키), ids 명령(`ids add --function`), inventory(함수 선언 위치)를 넓힌다. 이름을 바꾸면 ids rename(별칭)으로 ID를 잇는다 — UE의 이름 기반 참조·`CoreRedirects`를 안정 ID로 대신한다. ID 공간은 타입·필드와 같은 저장소 전체 유일 공간이다.
- **FD4 매개변수 형식:** 필드 값 종류 표(리플렉션 B9 — `ValueKind`)를 쓰되 frame(호출자 버퍼)에 값으로 만들 수 있는 것만 받는다.
  - 값(`T`) 또는 `const T&` 입력: 정수 8종·`bool`·`float`·`double`, `String`·`StringView`(같은 Utf8String 형식 — frame은 `String`으로 잡고 view를 넘긴다), `Name`, `Text`, `Guid`, `AssetId`, `Sha256Digest`, ROCKS_ENUM enum, 강한 참조가 없는 ROCKS_STRUCT·Binds 미러 struct(수학 타입 `const Vector3<double>&` 등), `ObjectHandle`(대상 `rocks::Object` — 최상위 칸만, 개정 4. 컨테이너 원소는 `WeakObjectRef<Object>`), `WeakObjectRef<T>`, `SoftObjectRef<T>`, `ClassRef<T>`, 위 원소의 `Array<T>`·`OrderedMap`·`OrderedSet`·`Optional<T>`(`const&`). 매개변수 이름은 ASCII 식별자다.
  - 값(`T`)으로 받는 매개변수는 복사 생성 가능해야 한다(개정 4 — `Invoke`는 같은 frame으로 여러 번 부를 수 있어 thunk가 인자를 옮기지 않고 복사로 넘긴다, 생성 static_assert RMC2704). 복사할 수 없는 `String`·컨테이너·`SoftObjectRef`는 `const&`로 받는다(UE도 `FString`·`TArray`를 `const&`로 받는다). `StringView`는 값이다(frame의 `String`에서 view를 넘긴다).
  - 거부(RMC27xx): `ObjectRef<T>`와 강한 참조를 담는 struct·컨테이너(`ObjectRefArray`·`ObjectRefMap`·`StructRefArray` — 복사·이동이 금지되고 소유 객체 밖에 둘 수 없다, `ObjectRefs.h:175-206`. 참조 목록은 `Array<WeakObjectRef<T>>`로), `InstancedStruct`, C 배열, 포인터, 비 `const` 참조 `T&`(UE 출력 매개변수 — 요구 시), rvalue 참조, 기본 인자(`= expr` — UE는 편집기 메타 `CPP_Default_*`로만 둬 cooked에서 쓸 수 없다, Rocks는 필요해지면 실행 schema에).
- **FD5 반환:** `void`(typedef 포함), FD4의 값 형식 `T`(참조 반환·강한 참조 형식은 오류, `StringView`도 오류 — 호출이 소유하지 않는 저장소를 가리키므로 `String`을 돌려준다, 개정 4), `Result<void>`, `Result<T>`. schema는 반환 형식과 `ReturnsResult` 표지를 싣는다(rocks-d2 3 — 스크립트는 실패를 `Result`로 받는다). 호출 결과는 두 층이다: 호출 장치의 실패(FD8 오류·인자 형식·할당)는 `FunctionCall::Invoke`의 `Result<void>`, 함수 자신이 돌려준 실패는 frame의 결과 오류 코드(`FunctionError()`)다.
- **FD6 flag(`FunctionFlags` — 실행 schema):** `Static`(C++ `static`), `Const`(C++ 끝 `const` — UE `FUNC_Const`), `Pure`(지정자 — 관찰 가능한 상태를 바꾸지 않는다, UE `BlueprintPure`. 반환값이 없으면 오류 — `UhtFunction.cs:849-851`), `EditorOnly`(`#if ROCKS_WITH_EDITOR` 안 — BuildTargets BD6, 지정자 필수), `ReturnsResult`(FD5). UE는 반환값 있는 `const` 함수를 자동으로 Pure로 보지만(`UhtFunction.cs:700-713` — BP 노드 모양 규칙) Rocks는 **명시한 `Pure`만** 인정한다: Sparks `pure`·UI `@pure fn`이 효과 검사의 근거라 `mutable` 캐시·입출력을 하는 `const` 함수를 순수로 보증하면 안 된다. getter·setter 구분(rocks-d2 7)은 `Const`(읽기 해석 — FD8)와 `Pure`가 한다.
- **FD7 호출 frame(UE `Parms` struct·`ProcessEvent` 대응):** 메타 컴파일러가 함수마다 생성 struct `<클래스>_<함수>Frame`(매개변수 값 멤버 + 결과 칸 + 결과 오류 코드)을 만든다. 실행 schema는 frame 크기·정렬과 매개변수마다 offset을 싣고, 생성 ops는 `construct(frame, allocator)`·`destroy(frame)`·`invoke`·`invokeBase`(FD13 예약)를 준다. 호출자가 frame을 할당·생성·파괴한다(UE와 같다 — `ScriptCore.cpp:2055-2254`, `CallFunctionByNameWithArguments` `:1396-1527`). 참조 매개변수(`const T&`)도 frame에 값으로 두고 참조를 넘긴다(UE `ConstParm|ReferenceParm` — 입력).
- **FD8 대상과 문맥(rocks-d2 1·2):** thunk는 raw 포인터가 아니라 `(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle self)`로 대상을 받는다 — 필드 ops와 같은 모양. `Const` 함수는 읽기 해석(`Resolve` — 읽기 Borrow), 그 밖은 쓰기 해석(`ResolveForWrite` — encode 고정 중이면 `Busy`, `MutationStamp`를 올려 대입 묶음의 변경 감지에 들어간다, `ObjectRegistry.h:254-270`)이다. 오류는 Registry 경로 그대로다(crash 없음 — UE는 garbage 대상이면 조용히 return한다, `ScriptCore.cpp:2068-2071`): 다른 스레드 `WrongThread`, 다른 접근 문맥 `WrongContext`(World 게임 스레드 밖 — `ObjectAccessScope`), 사라진 handle `StaleHandle`, 파괴 요청·Discard·cutoff `Destroyed`, 타입 다름 `TypeMismatch`, 쓰기 해석의 encode 고정 `Busy`, 종료 중 `ShuttingDown`(`ObjectRegistry.cpp:122-150`). `static` 함수는 대상이 없어도 같은 스레드·문맥 검사(Registry `CheckAccess`)를 받고, handle은 null이어야 한다(아니면 `InvalidArgument`). World의 파괴 지연(`World::DestroyActor` — 프레임 끝에 OnDestroy·회수)은 Registry `RequestDestroy` 전이라 Registry 상태로는 살아 있다: 그 사이의 동작(SL 10.2 함수 표 "PendingDestroy 대상에서의 동작")은 함수와 host가 정한다.
- **FD9 범용 호출 API(ObjectRuntime `Object/FunctionCall.h`):** `FindFunction`이 돌려준 `FunctionHandle`(함수 schema·ops와 소유 타입의 `TypeLease` — 호출 중 Unregister·모듈 reload가 thunk·schema를 무효로 만들지 않게, `RuntimeTypeOps.h:318-321`)로 `FunctionCall call(allocator, function)`을 만든다. 매개변수마다 합성 `FieldSchema`(이름·값 형식·flags·원소 상한 — id는 FunctionId와 매개변수 번호에서 결정적으로 만든 값이고 저장하지 않으므로 ID 명세에 없다)와 `RuntimeValueOps`(`RuntimeTypeOps.h:591`)를 두어 필드 쓰기·분리된 값 경로 도구(`DetachedValue`·`PathTarget::Value`)를 그대로 쓴다. 쓰기: `SetArgument(registry, access, index, const FieldValue&)`(스칼라·문자열·참조·enum — 필드 쓰기와 같은 변환·검사: weak 대상 생존·기대 타입, Name 토큰 pool, ClassRef 등록 여부), `SetArgumentValue(index, const void* source, const void* nativeType)`(struct·컨테이너 매개변수 — 같은 C++ 형식(`NativeTypeKey`)의 값을 값 ops의 `copy`로 넣는다, 개정 3. 분리된 값 위의 경로 편집은 요구 시). `Invoke(registry, access, self)`. 결과: `ReadResult(FieldValue&)`(단일 값)·`ResultValue(nativeType)`(struct·컨테이너 — 같은 형식일 때 그 값의 주소)·`FunctionError()`. 쓰지 않은 인자는 형식 기본값이다(frame 생성이 값 초기화). 할당 실패는 `OutOfMemory`로 끝나고 frame은 소멸자에서 정리한다.
- **FD10 런타임 등록:** `TypeSchema::functions`(`Span<const FunctionSchema>` — 자기 함수만, 상속은 기반 체인)와 `RuntimeTypeOps::functions`(생성 ops). `TypeRegistry`가 깊게 복사하고 `FindFunction(TypeId, Guid)`·`FindFunctionByName(TypeId, name)`(기반 체인 포함, 대소문자 무시)을 준다. 생성 등록 코드가 함수 표를 함께 등록한다. 매개변수의 enum·struct 형식은 필드처럼 레지스트리 레코드에 잇지 않는다(개정 3 — 생성 setter가 C++ 형식으로 enum ID·선언 값을 보고, struct는 같은 형식의 값만 받는다). descriptor(`.rocksmeta.json`)에 함수(이름·ID·flag·매개변수·반환)를 실어 다른 모듈의 상속 이름 검사와 Sparks binding이 쓴다 — descriptor schema 5(7절).
- **FD11 서명 비교(rocks-d2 6, UE `IsSignatureCompatibleWith` — `Class.cpp:7831-7867`):** `FunctionSignatureMatches(const FunctionSchema&, Span<const ValueType> parameters, ValueType result, bool returnsResult)` — 매개변수 수·순서·형식(이름 무시, 값·`const&` 차이 무시, `StringView`와 `String`은 같은 형식)과 반환이 같아야 한다. UE는 바인딩 때 검사하지 않지만 Rocks는 FR4 바인딩이 이 함수로 바인딩 시점에 거부한다.
- **FD12 지정자 이름 공간:** FR1 — `Pure`·`EditorOnly`·`Name`. 예약(뒤 단계가 받는다, 지금은 RMC2000 모르는 지정자): `DisplayName`·`Category`·`Help`(FR2), `ScriptOverridable`(FR3), `Cheat`·`Authority`(CVR8), Sparks 함수 지정자(EI1b — `Lane`·`Effects` 등). 네트워크(`Server`·`Client`·`NetMulticast`)는 네트워크 단계. 치트 함수의 `Help`는 에디터 데이터가 없는 개발 게임 빌드의 콘솔 도움말이 쓰므로 편집 메타 블록이 아니라 실행 schema(또는 CVR8이 생성하는 치트 표)에 남는다 — UiToolkit 7.3 (b)의 "실행 schema에 남는 Help" 목록에 더한다(설정 struct·명령줄 바인더와 같은 이유).
- **FD13 스크립트 재정의(rocks-d2 5):** FR3의 제약은 UiToolkit 7.3 (f)가 정본이다(`ScriptExtensible` 클래스의 `virtual` 함수만, 순수 가상은 `Abstract` 클래스에만·반환 `void`·`Result<T>`, `final` 함수·클래스 불가 — UE `UhtFunction.cs:836-838`, Gameplay가 `final`로 닫은 hook은 재정의 대상이 아니다). `invoke`는 가상 호출이라 대리 클래스를 거친다: Sparks `override fn`이 기반 구현을 부를 때 다시 자기에게 돌아오지 않도록 `RuntimeFunctionOps::invokeBase`(비가상 `X::F` 호출 — UE `_Implementation` 직접 호출 대응)를 FR1에서 칸만 예약한다(null).
- **FD14 검사(메타 컴파일러 RMC27xx — 메시지 영어):** 대상·자리·접근 무관·`noexcept`(FD1), 이름(FD2), 형식(FD4·FD5), flag 조합(`Pure` 반환 없음), EditorOnly 블록 일치(BD6)와 BuildTargets 3.4의 두 변형 AST 비교에 함수 포함(차이는 정확히 EditorOnly 필드·함수), 모듈 사이 상속 함수 이름 겹침(descriptor), ID 없음(RMC2100).

## 3. 런타임 모양(FR1)

```cpp
// MetaSchema Meta/TypeSchema.h
enum class FunctionFlags : uint32 { None = 0, Static = 1, Const = 2, Pure = 4, EditorOnly = 8, ReturnsResult = 16 };

struct ParameterSchema {
    Span<const char> name;
    ValueType value{};       // FieldSchema와 같은 규칙(value·element·key)
    ValueType element{};
    ValueType key{};
    uint32 flags = 0;        // FieldFlags 부분 집합(Required — null 불가 참조, SL 10.2 nullability)
    uint32 maxElements = 0;  // 컨테이너 원소 상한(없으면 MaxArrayElements)
    uint32 offset = 0;       // frame 안 위치
};

struct FunctionSchema {
    Guid id;
    Span<const char> name;      // C++ 이름
    Span<const char> toolName;  // Name = "…"(비면 name)
    uint32 flags = 0;           // FunctionFlags
    Span<const ParameterSchema> parameters;
    ParameterSchema result{};   // void면 value.kind == Opaque
    uint32 frameSize = 0;
    uint32 frameAlignment = 0;
};

// ObjectRuntime Object/RuntimeTypeOps.h
struct RuntimeFunctionOps {
    Guid functionId{};
    Result<void> (*construct)(void* frame, Allocator&) noexcept = nullptr;
    void (*destroy)(void* frame) noexcept = nullptr;
    Result<void> (*invoke)(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle self, void* frame) noexcept = nullptr;
    Result<void> (*invokeBase)(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle self, void* frame) noexcept = nullptr; // FR3
    Span<const RuntimeValueOps* const> parameters; // 매개변수 값 ops(FD9)
    const RuntimeValueOps* result = nullptr;
};
```

생성 thunk(개념 — 함수 `SceneComponent::SetMobility(ComponentMobility) noexcept -> Result<void>`):

```cpp
// SceneComponent_SetMobilityFrame { ComponentMobility mobility; ErrorCode error; }
static Result<void> Invoke_SetMobility(ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle self, void* frame) noexcept {
    auto target = registry.ResolveForWrite<SceneComponent>(access, self); // const 함수는 Resolve
    if (!target) { return Result<void>::Failure(target.Error()); }        // WrongThread·WrongContext·StaleHandle·Destroyed·TypeMismatch·Busy·ShuttingDown
    auto& f = *static_cast<SceneComponent_SetMobilityFrame*>(frame);
    auto done = target.Value()->SetMobility(f.mobility);
    f.error = done ? ErrorCode::None : done.Error();
    return Result<void>::Success();
}
```

## 4. 요구 대응

| 요구(rocks-d2) | 대응 |
|---|---|
| 1 대상 수명 | FD8 — handle로 받고 Registry 오류(`StaleHandle`·`Destroyed` 등), crash 없음. World 파괴 지연 동안은 함수·host 정책 |
| 2 스레드 표지 | FD8 — 기존 접근 문맥 장치(`WrongThread`·`WrongContext`)가 모든 호출(static 포함)에 걸린다. 별도 flag는 두지 않는다(실행 의미가 없다) |
| 3 Result 반환 | FD5 `ReturnsResult`와 frame 결과 오류 코드 |
| 4 인자 타입 | FD4 — `ObjectHandle`·`WeakObjectRef<T>`·`SoftObjectRef<T>`·`Transform`·`Vector3<double>`·enum·강한 참조 없는 struct `const&`(`ObjectRef<T>`는 필드 전용 — 매개변수로는 `WeakObjectRef<T>`·`ObjectHandle`) |
| 5 스크립트 재정의 | FD13 — FR3, `final` 제외, `invokeBase` 예약 |
| 6 함수 바인딩 | FD11 서명 비교 + FR4 |
| 7 순수·const | FD6 `Const`·`Pure`(명시만) |

## 5. 시험(FR1)

- C# meta: 받는 선언(인스턴스·static·const·private·EditorOnly·리플렉션하지 않은 오버로드 옆)마다 descriptor·생성 schema golden, 매개변수 형식 표 전부와 거부 형식, 거부 사례마다 RMC 코드·위치(FD14), 모듈 사이 상속 함수 이름 겹침, ID 명세의 `function` C++ 이름 키.
- C++: 생성 thunk로 리플렉션 fixture 함수를 `FunctionCall`로 불러 값·`Result` 오류·Registry 오류 일곱(FD8)·static 호출과 non-null handle, const 함수의 읽기 해석(encode 고정 중 성공)과 비 const의 `Busy`, struct·문자열·컨테이너 인자, 할당 실패 전수(누수 없음), `FindFunction`·이름 조회의 상속 체인, 호출 중 lease 유지, `FunctionSignatureMatches` 양성·음성.

## 6. 버린 대안

- **FieldValue 배열로 인자를 넘기는 thunk(frame 없음):** struct·컨테이너 인자를 FieldValue가 담지 못해 경로가 둘로 갈린다. UE처럼 frame 하나에 모든 매개변수를 두고 쓰기는 필드 쓰기 도구를 재사용한다.
- **raw 포인터 대상:** World 객체는 언제든 파괴되므로 호출 경로마다 생존 판정이 흩어진다(rocks-d2 1).
- **오버로드 허용(이름 + 서명으로 식별):** 스크립트·텍스트 참조가 이름 하나로 함수를 가리켜야 하고 UE도 막는다.
- **필드와 함수 이름 공간 공유(UE shadowing):** Rocks getter 관례(필드 도구 이름 = getter 이름)를 리플렉션할 수 없게 된다. 노출 이름 공간의 겹침은 Sparks binding이 본다.
- **자동 Pure(UE):** 효과 검사의 근거로 쓰기에 `const`는 약하다.
- **출력 매개변수 FR1 포함:** Sparks·UI 변환·치트가 쓰지 않고 frame 쓰기 방향이 늘어난다 — 요구 시.

## 7. 구현(FR1)

- **FR1a 런타임(MetaSchema·ObjectRuntime):** `FunctionFlags`·`ParameterSchema`·`FunctionSchema`·`TypeSchema::functions`(`Meta/TypeSchema.h`), `RuntimeParameterOps`·`RuntimeFunctionOps`·`RuntimeTypeOps::functions`(`Object/RuntimeTypeOps.h`). `TypeRegistry` 등록이 함수 표를 검사(`Private/TypeRegistryFunctions.cpp` — 개수·flag·frame 크기·정렬·offset·연산 짝·이름 유일, 기반 체인 이름 겹침은 lock 안에서)하고 깊게 복사한다. struct 등록은 함수를 받지 않는다. 조회는 `FindFunction(TypeId, Guid)`·`FindFunctionByName(TypeId, name)`(기반 체인, 도구 이름, 대소문자 무시). 호출은 `Object/FunctionCall.h`의 `FunctionHandle`(소유 타입 `TypeLease`)·`FunctionCall`(Prepare·SetArgument·SetArgumentValue·Invoke·ReadResult·ResultValue·FunctionError, frame 태그 `FCAL`)이고 생성 코드용 도우미 `detail::WriteParameter`·`ReadParameter`·`ParameterOpsOf`·`ConstructFunctionFrame`·`DestroyFunctionFrame`이 같은 헤더에 있다.
- **FR1b 메타 컴파일러:** `ROCKS_FUNCTION(...)`(`Meta/Annotations.h` — annotation `rocks.function`). 해석은 `ClangAstParser.Functions.cs`(자리·모양·오버로드·접근, RMC2701·2702), 정규 형식은 탐침 `RocksFunctionShape<decltype(&Owner::Func)>`(`CanonicalTypeProbe.cs` — 생성 코드 검증이 컴파일러 내장 함수를 막으므로 pack 원소는 재귀 템플릿 `RocksCanonPack`), 칸 형식·Pure·이름은 `SchemaValidator`(필드 값 종류 판정 `ResolveKind`를 함수 칸과 공유 — `Result<T>` 풀기, cv·참조 벗기기, `StringView` → Utf8String, `ObjectHandle` → WeakObjectReference(rocks::Object), RMC2703–2705), EditorOnly 자리(`ConditionalScan` 에디터 블록 — RMC2706)와 두 조합 비교(이름·static·const·지정자·쓴 형식 철자 — RMC2503)는 `CompilerDriver`. 생성은 `CppEmitter.Functions.cs`: frame struct `detail::generated_frames::RocksFrame_<길이 접두 마디>`(`game::Door::Open` → `RocksFrame_4game4Door4Open` — `_`로만 이으면 `Door::Open_Fast`와 `Door_Open::Fast`가 겹쳐 다른 생성 소스와 같은 외부 이름이 된다), 칸 `detail::FrameSlot<T>`(정렬된 byte 저장소 — 값 형식이 표준 배치가 아니어도 frame이 표준 배치라 `offsetof`가 맞다) `p0…` → `result` → `error`, allocator 생성자가 선언 순서로 만들고 소멸자가 거꾸로 부순다, thunk `GeneratedType<T>::RocksInvoke_<함수>`, 실행 schema·연산표 배열(끝 표지 — 모든 함수가 EditorOnly인 게임 조합에서도 크기가 0이 아니다), EditorOnly 함수는 `#if ROCKS_WITH_EDITOR`, 형식 static_assert(강한 참조 struct·복사할 수 없는 값 매개변수 — RMC2704). ID 명세 `function` kind·`ids add --function`·inventory(함수 선언 위치).
- **descriptor schema 5:** 클래스 항목 `functions` 배열(선언 순서, 자기 함수만) — `name`·`id`·`toolName`(실효)·`flags`(`Static`→`Const`→`Pure`→`EditorOnly`→`ReturnsResult`)·`parameters`(`name`과 필드와 같은 형식 칸 `value`·`element`·`key`·`maxElements`)·`result`(형식 칸 넷 또는 null). reader는 필드와 같은 형식 규칙(`ReadSlotType` 공유)에 frame에 둘 수 없는 종류·`Static`+`Const`·반환 없는 `Pure`·이름 겹침·상한을 거부한다(RMC1000).
- **시험:** C# meta `MetaTests.Functions.cs`(받는 선언의 descriptor·생성 코드·두 조합 컴파일, RMC2701–2706·RMC2503 거부 묶음), `ModuleMetaTests.Functions.cs`(descriptor 왕복, import 기반 함수와 이름 겹침, reader 엄격성). C++ `FunctionCallTests.cpp`(손으로 만든 표 — 등록 거부·상속 조회·lease·할당 실패), `FunctionCallGeneratedTests.cpp`(fixture `ReflectionTestFixtures/FunctionFixture.h`의 생성 표 — 인스턴스·const·static·Result·함수 실패·StringView·enum·struct·Array·handle·private·EditorOnly·Registry 오류).
- **남은 것:** `FunctionSignatureMatches`(FD11)는 FR4 바인딩과 함께, 매개변수 flag `Required`는 Sparks nullability(EI1b)와 함께, 읽기 해석의 encode 고정 중 성공·비 const `Busy` 시험은 encode 고정을 여는 시험 도우미가 생길 때.
