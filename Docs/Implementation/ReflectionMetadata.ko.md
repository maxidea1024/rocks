# Reflection과 Clang 메타 컴파일러 구현

이 문서는 현재 구현한 Reflection 범위와 사용 계약을 정리한다. 엔진 런타임은 자체 타입·컨테이너를 사용하고, 메타 컴파일러는 호스트 C#/.NET 10과 네이티브 Clang을 사용한다. 전체 엔진 메타 빌드 시스템, 에디터 프로퍼티 UI, 자동 스키마 마이그레이션 생성은 아직 구현 범위에 포함하지 않는다. ObjectGraph의 명시 migration callback과 그 출력 검증은 별도 Serialization 계층에서 제공한다.

## 타입 조회의 현재 상태와 사용성 목표

생성 헤더를 포함한 리플렉션 타입은 `rocks::TypeIdOf<T>()`로 정적 ID, `rocks::TypeOf<T>()`로 생성된 정적 schema를 조회한다. 정적 schema는 등록된 타입 lease가 아니다. schema와 이름 참조는 생성 코드가 포함된 모듈의 수명에 종속된다. 생성 헤더 누락 또는 리플렉션하지 않은 타입은 컴파일 시 진단한다.

유효한 Borrow로 접근 중인 객체는 `object->GetType()`으로 실제 등록 타입의 `TypeLease`를 얻는다. 호출 실패를 직접 처리하려면 `object->TryGetType()`을 사용한다. `GetType()`은 오류를 상태로 추측하지 않고 원인을 진단한 뒤 중단한다. 객체는 유효한 Borrow가 필요하며, 문맥·소유 스레드·생존 상태를 검사한다. 파괴 요청 후에는 새 타입 lease를 얻을 수 없다.

```cpp
// MyObject의 메타컴파일러 생성 헤더를 포함했다고 가정한다.
auto staticId = rocks::TypeIdOf<MyObject>();
const auto& staticSchema = rocks::TypeOf<MyObject>();

// object는 유효한 Borrow 안에서 얻은 포인터다.
auto type = object->GetType();
auto runtimeId = type.Id();
auto runtimeName = type.Name();
// runtimeName과 type.Schema()는 type 또는 같은 레코드의 다른 lease보다 오래 보관하지 않는다.
```

`TypeLease::Id()`는 값이다. `Name()`, `Schema()`, `Ops()`는 lease에 종속된 뷰·참조이며, 임시 lease에서 호출하면 바로 무효화될 수 있어 컴파일 시 거부한다. 예를 들어 `object->GetType().Id()`는 가능하지만, 이름을 보관할 때는 위 예처럼 lease를 변수에 둔다. lease는 타입의 등록과 모듈 코드 수명을 유지하며 객체 수명을 연장하지 않는다. Registry와 allocator는 lease보다 오래 살아야 한다.

클래스의 정적 조회는 `T::StaticType()`처럼 직접 사용하는 형태도 지향한다. 일부 reflected 타입에는 현재 생성 본문 매크로가 없고, 접근 지정자 변경·생성 헤더 순환·파생 타입의 기반 정적 함수 상속을 검증해야 하므로 이 멤버 API는 아직 지원하지 않는다. 타입 제한 클래스 참조 및 전체 메타클래스 기능도 아래 후속 범위다.

## 클래스 메타객체의 범위

현재 `TypeSchema`, `RuntimeTypeOps`, `TypeRegistry`, `TypeLease`는 등록된 타입에 관한 정보, 추상 여부·상속 메타데이터 및 모듈 수명 보호의 기반을 제공한다. 등록된 `TypeId`를 쓰는 `ObjectRegistry::NewObject<Object>(access, typeId)`는 런타임 타입을 지정하는 낮은 수준의 객체 생성 경로다. 이 경로를 타입 제한 클래스 참조나 자동 에디터 선택, 클래스 기본 객체와 동일한 완성 기능으로 해석하지 않는다.

클래스 자체를 데이터로 전달·저장하고 기반 타입 제한, 동적 생성, 기본값·필드·함수와 에디터·에셋 연결을 지원하는 기능은 [런타임 클래스 메타데이터 설계](../Design/RuntimeClassMetadata.ko.md)의 후속 목표다. 실제 C++ 구현·사용 예제·플랫폼별 테스트가 완료된 부분만 이 문서의 현재 상태로 기록한다.

## 선언과 지원 형식

`ROCKS_CLASS(...)`를 `class`/`struct` 앞에, `ROCKS_PROPERTY(...)` 또는 `ROCKS_NATIVE()`를 필드 앞에 직접 작성한다. 선언에 안정 ID를 넣지 않는다(스키마 버전은 `Version=N` 지정자로 정한다). 매크로와 타입 사이에는 공백과 주석만 허용하고 전처리 지시문·별도 선언·wrapper 매크로는 거부한다. 조건부 선언은 마커와 클래스를 같은 `#if` 블록 안에 둔다.

리플렉션 헤더를 가진 모듈은 모듈 폴더에 identity 명세 `<ModuleName>.rocksids.json`을 둔다. 명세는 그 모듈 타입과 필드의 무작위 안정 ID, 이전 이름과 폐기 이력을 보관한다. 새로운 선언의 ID 발급과 이름 변경·폐기는 `ids add`, `ids rename`, `ids retire`를 사용하는 명시적인 저작 작업이다. 타입을 다른 모듈로 옮길 때는 `ids move --from <명세> --to <명세> --type <이름>`이 타입과 그 필드 항목을 ID 그대로 옮긴다. 대상에 먼저 붙이고 원본에서 떼므로 중간에 멈추면 같은 명령을 다시 실행한다. 일반 빌드는 명세를 읽기만 하고 누락 또는 중복을 거부한다. 모듈마다 생성 결과의 inventory로 활성 명세의 누락과 중복을 검사하고(`ids verify`), 타깃에서 도달하는 모듈의 모든 명세(은퇴 항목만 남은 예약 명세와 SDK가 동봉한 명세 포함) 사이에 같은 ID나 같은 종류의 같은 이름·별칭이 있으면 거부한다(`ids verify-set`, RMC2101 — 한 ID·한 이름은 한 명세에만 있다). 이 검사가 모두 끝나야 생성물을 게시한다. 리플렉션 헤더는 정확히 한 모듈이 소유한다. 미등록 헤더의 자동 탐색은 아직 없다.

이력 검사(`ids merge-check`, `Build/CheckIdentityMerges.ps1`)는 리비전마다 저장소의 모든 명세를 ID 기준으로 합쳐 비교한다. 따라서 명세를 옮기거나 타입을 다른 명세로 옮겨도 ID가 사라진 것으로 보지 않으며, ID 삭제·재사용과 예약 이름 재사용은 계속 거부한다. 합치기 전에 한 리비전 안의 두 명세에 같은 ID나 이름이 있는지 보고, 있으면 측면과 파일 경로를 함께 보고하고 실패한다(빌드의 `ids verify-set`과 같은 규칙).

저장 레코드에는 그 타입의 스키마 버전이 기록된다. 버전은 `ROCKS_CLASS(Version=N)`(없으면 1)이며 영속 필드의 의미·형식이 바뀌면 올리고 마이그레이션을 등록한다. base의 버전은 파생 Record에 기록되지 않으므로 base의 영속 배치를 바꾸면 파생 타입들도 버전을 올린다(MetaCompiler는 강제하지 않는다). 사용자 정의 스키마 변경의 자동 판정과 마이그레이션 생성은 아직 구현되지 않았다. 이름 변경 시 명세의 `ids rename`으로 ID를 유지하며, 다른 ID를 새로 발급해 이전 필드를 대체하면 활성 이전 선언이 누락되어 빌드가 실패한다.

| 필드 형식 | 런타임 표현 |
| --- | --- |
| `bool` | `ValueKind::Bool` |
| `uint8`, `uint16`, `uint32`, `uint64` | 폭에 대응하는 부호 없는 정수 |
| `int32`, `int64` | 폭에 대응하는 부호 있는 정수 |
| `float`, `double` | `Float32`, `Float64` |
| `Guid` | 두 개의 64비트 정수로 구성된 값 |
| `String` | 소유한 유효 UTF-8 문자열; 길이는 바이트 수 |
| `ObjectRef<T>` | `ObjectHandle`을 담은 참조 값; 실제 변경은 Registry barrier 경유 |
| `AssetId` | `ValueKind::AssetId`; 에셋 ID(`Object/AssetId.h`, Guid, 0 = null). 저장 wire는 Guid |
| `Sha256Digest` | `ValueKind::Digest256`; 32 byte 해시(`Core/Encoding/Sha256.h`). 저장 wire는 정확히 32 byte의 Bytes |
| `SoftObjectRef<T>` | `ValueKind::SoftObjectReference`; 에셋 ID와 패키지 안 객체 경로(`Object/SoftObjectRef.h`). GC 간선이 아니며 `value.type`이 기대 타입(Object면 0). 저장 wire는 `SoftObjectReference`(12) |
| `ROCKS_ENUM` enum class | `ValueKind::Enum`; `value.type`이 enum ID. 값은 선언된 값(Flags enum은 선언 값의 OR)만 유효하다. 저장 wire는 I64 (아래 "리플렉션 enum") |
| `ROCKS_STRUCT` struct | `ValueKind::Struct`(`FieldKind::Value`); `value.type`이 struct ID. 저장 wire는 중첩 Record(10) (아래 "리플렉션 struct") |

스키마의 값 형식은 `FieldSchema::value`(`ValueType { ValueKind kind; TypeId type; }`)다. `type`은 강한 참조와 소프트 참조의 기대 대상 타입(0 = Object)과 Enum의 enum ID·Struct의 struct ID(0이면 안 된다)이며 그 밖의 종류에서는 0이어야 한다. `element`·`maxElements`는 배열 필드(B5)용 자리이며 지금은 Opaque·0만 등록된다.

`SoftObjectRef<T>`의 규칙: null은 asset이 0이고 경로가 빈 것이다(asset만 있고 경로가 비면 그 에셋의 대표 객체다). asset이 0인데 경로가 있거나, 경로가 4 KiB(`MaxObjectPathBytes`)를 넘거나 UTF-8이 아니면 InvalidArgument다. 리플렉션 쓰기(`write`·`writeStaged`·등록 전 callback), 그래프 저장(직접 대입한 값도 다시 검사)과 로드가 같은 규칙을 검사한다. 경로 문법(Outer 이름을 `.`로 잇는다)과 기대 타입은 대상을 찾을 때(A0-O·A1) 검사하며 쓰기·저장·로드는 검사하지 않는다. 이동 전용이고 이동하면 원본은 null이 된다. 이동 대입은 원본의 경로 allocator를 넘겨받으므로 객체 필드에 값을 넣을 때는 `TryAssign(other)`(받는 쪽 allocator 유지)를 쓰고, 복제는 실패할 수 있는 `TryClone(allocator)`다. 쓰는 클래스에는 `noexcept T(Allocator&)` 생성자가 필요하다(String 필드와 같다). `AssetId`의 문자열 표기와 해석은 36자 소문자 하이픈 형식 하나뿐이다(`TryParse`는 대문자·32자·중괄호를 거부한다).

현재 생성 대상은 이름 있는 비템플릿 타입이며, 모든 비정적 필드는 `ROCKS_PROPERTY`(영속·리플렉션 필드) 또는 `ROCKS_NATIVE`(비영속 런타임 멤버) 중 하나를 단다(둘 다면 RMC2204). private/protected 필드는 해당 리플렉션 클래스 안에 `ROCKS_GENERATED_BODY()`를 직접 한 번 선언해야 한다. 리플렉션하지 않은 클래스, 다른 매크로로 감싼 호출, 수동 작성한 내부 annotation과 중복 선언은 거부한다. PROPERTY 필드에서 raw pointer, 배열·컨테이너, enum, const/volatile 필드는 거부한다(NATIVE 멤버는 강한 객체 참조만 아니면 이런 타입을 둘 수 있다). bitfield와 annotation을 붙인 static 필드는 거부한다. static 보조 데이터는 annotation 없이 둘 수 있다. 수동 `Trace`와 자동 추적을 섞어 완전한 필드 레이아웃으로 오인하지 않도록, 생성 대상의 직접 선언된 `Trace`는 거부한다.

단일 public 비가상 상속만 지원한다. 기반 타입과 `ObjectRef<T>`·`SoftObjectRef<T>`의 대상은 `Object`, 같은 모듈의 리플렉션 타입, 또는 import한 의존 모듈 descriptor의 public 리플렉션 타입이어야 한다(아래 "모듈 메타데이터"). 파생 스키마는 상속 필드를 안정 ID 순서로 펼치며, 상속을 포함한 필드 수는 최대 4,096개다. 생성 접근기는 멤버 포인터를 사용한다. 객체 레이아웃에서 offset을 추정하거나 raw 필드 주소를 반환하지 않는다.

### 속성 지정자와 표시 문자열

```cpp
ROCKS_CLASS()
class Character : public rocks::Object {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY(ReadOnly,
                   Category = "능력", DisplayName = "체력")
    rocks::int32 health = 100;
};
```

지정자 값은 JSON escape 문자열 또는 부호 없는 10진 정수(앞자리 0 금지, 1–2³¹−1)다. 중복·알 수 없는 지정자, 값이 필요한 지정자의 값 누락과 잘못된 형식은 거부한다.

| 지정자 | 대상 | 뜻 |
|---|---|---|
| `Abstract` | CLASS | 등록할 수 있으나 생성할 수 없다(`TypeFlags::Abstract`). |
| `Version=N` | CLASS | 스키마·Record 버전(없으면 1). |
| `WorldOnly` | CLASS | `TypeFlags::WorldOnly`. **상속된다** — base가 WorldOnly면 파생도 자동으로 WorldOnly다(TypeRegistry도 수동 등록에서 확인). 에셋 객체 Registry의 생성 거부는 패키지 단계(A0-O)의 정책이다. |
| `Transient` | PROPERTY | GC에서 추적하지만 저장 그래프에서 뺀다. |
| `ReadOnly` | PROPERTY | 일반 write는 거부하고 생성·복원의 staged write만 허용한다. |
| `EditorOnly` | PROPERTY | 플래그만 단다(항상 컴파일). 쿠킹 데이터에서 빼는 것은 graph v2(A0-O)이며 지금 graph는 보통 필드처럼 저장한다. |
| `Required` | PROPERTY | `ObjectRef<T>`·`SoftObjectRef<T>` 필드에만 쓴다(그 밖은 RMC2202). 패키지 저장·쿠킹이 null을 거부한다(A1). 로드는 거부하지 않는다. |
| `Name="…"` | PROPERTY | 도구·명령행 이름(`FieldSchema::toolName`). ASCII 식별자이며, 없으면 C++ 필드 이름이다. 상속 필드를 포함해 대소문자 무시로 유일해야 한다(RMC2203). 직렬화는 필드 ID를 쓰므로 이름과 무관하다. |
| `Category="…"`·`DisplayName="…"` | PROPERTY | 표시용 UTF-8 문자열(아래). |
| `MaxElements=N` | PROPERTY | 배열 길이 상한. 배열 필드는 B5에서 생기므로 지금은 모든 필드에서 RMC2202다. |

`ROCKS_NATIVE()`는 저장·리플렉션하지 않는 런타임 멤버(GPU 핸들·캐시)이며 지정자를 받지 않는다. GC가 추적하지 않으므로 강한 객체 참조를 담을 수 없다: MetaCompiler가 Clang AST로 멤버 타입을 값으로 담는 필드·base·이름 없는 struct/union 멤버·배열 원소·템플릿 인자와 패턴 필드까지 재귀 검사해 `ObjectRef`·`RootRef`·`ObjectRefArray`나 Object 파생 타입의 raw 포인터·참조가 있으면 RMC2205로 거부한다. 포인터·참조는 대상이 Object 파생이거나 대상이 다시 강한 참조(`T**`·`ObjectRef<T>*`)인지만 본다 — 빌린 포인터가 가리키는 비-Object 레코드(예: `World*`)의 필드는 그 소유자가 추적한다. Object 파생 타입은 값이나 템플릿 인자로 닿아도 펼치지 않고, `WeakRef`·`SoftObjectRef`·`ObjectHandle`은 허용한다(대상을 담지 않으므로 템플릿 인자도 따라가지 않는다). `static constexpr bool RocksRequiresWorldOnlyOwner = true;`를 선언한 타입(을 값으로 담는 타입)은 WorldOnly 클래스의 NATIVE 멤버로만 둘 수 있다(RMC2206). 생성 코드는 직접 멤버 타입(배열 원소·포인터와 참조의 대상 포함, 담는 타입의 필드는 제외)에 대해 같은 조건을 `static_assert`(`IsNativeStrongReference`·`RequiresWorldOnlyOwner`, `<Object/NativeMemberTraits.h>`)로 한 번 더 둔다. NATIVE 멤버는 영속 필드가 아니고 강한 간선을 담지 않으므로 생성 타입의 `completeFieldLayout`은 계속 true다. 수동 등록 매크로는 `ROCKS_NATIVE_TYPE`이다(이전 `ROCKS_NATIVE_CLASS`).

### 리플렉션 enum(`ROCKS_ENUM`)

```cpp
ROCKS_ENUM(ToolPrefix = "TEXTUREGROUP_")
enum class TextureGroup : rocks::uint8 {
    World = 0,
    EightBitData ROCKS_ENUMERATOR(Name = "8BitData", DisplayName = "8비트 데이터") = 7,
    Reserved13 ROCKS_ENUMERATOR(Hidden) = 13,
};

ROCKS_CLASS()
class TextureSettings : public rocks::Object {
public:
    ROCKS_PROPERTY() TextureGroup group = TextureGroup::World;
};
```

`ROCKS_ENUM(...)`은 바로 뒤의 enum 정의 하나에 붙는다(`ROCKS_CLASS`와 같이 사이에는 공백·주석만, 같은 소스·스코프). 클래스 안의 중첩 enum도 되지만 public 구역이어야 한다(아니면 RMC2401 — 생성 코드가 바깥에서 이름을 쓴다). 대상은 underlying을 enum 이름 바로 뒤에 원문으로 명시한 `enum class`(또는 `enum struct`)이고 underlying은 `uint8`·`uint16`·`uint32`·`int32`만 받는다(생략·`int8`·`int16`·64비트는 RMC2401). 열거자는 하나 이상, 4,096개 이하이며 값이 서로 달라야 한다(별칭은 RMC2401). 초기값이 없는 열거자는 이전 값 + 1이다. 지정자는 `ToolPrefix="…"`(`[A-Za-z0-9_]+`)와 `Flags`(비트 플래그 — 열거자 값은 음수일 수 없고, 유효한 값은 선언 값들의 OR이며 0은 항상 유효)다. 열거자에는 이름 바로 뒤에 `ROCKS_ENUMERATOR(Name="…", DisplayName="…", Hidden)`를 붙일 수 있다. `Name`은 도구 이름(`[A-Za-z0-9_]+`, 숫자로 시작해도 된다 — 접두를 뗀 UE 이름 `8BitData`)이고 없으면 C++ 이름이다. `DisplayName`은 표시용 UTF-8, `Hidden`은 저장 값으로는 유효하지만 도구 설정이 받지 않는 열거자다(판정은 설정 계층). 접두가 붙은 이름과 뗀 이름을 합친 도구 이름 집합은 대소문자 무시로 유일해야 한다(RMC2203). enum도 타입 ID를 identity 명세의 `type` 항목으로 가진다(`ids add --type <정규 이름>`). 열거자는 ID가 없다 — 저장은 값으로 하므로 열거자 이름을 바꿔도 데이터가 유지되고, 값을 바꾸면 저장된 데이터의 뜻이 바뀐다.

생성 헤더는 `GeneratedType<E>`에 `Id()`, `Schema()`(`EnumSchema` — 이름·underlying·플래그·접두와 열거자의 이름·도구 이름·표시 이름·값·플래그), constexpr `IsDeclared(int64)`, `Register(TypeRegistry&, ModuleLease = {})`를 낸다. C++ underlying이나 열거자 값이 바뀌었는데 생성물을 다시 만들지 않으면 생성 헤더의 `static_assert`가 실패한다(MetaCompiler가 Clang AST에서 읽은 값을 컴파일러가 한 번 더 대조한다). `IsDeclaredEnumValue(schema, value)`와 `FindEnumerator(schema, toolName)`(`Meta/TypeSchema.h`)은 같은 규칙의 실행 시 버전이다 — 도구 이름 조회는 ASCII 대소문자를 무시하고 접두가 있든 없든 받으며, 숨김 열거자도 찾는다.

enum은 그 enum을 필드로 쓰는 클래스보다 먼저 등록한다(`TypeRegistry::RegisterEnum`, 모듈 집계 함수는 enum을 먼저 등록한다). 클래스 등록은 Enum 필드의 `value.type`이 등록된 enum 레코드여야 하고(없으면 NotFound, 클래스 레코드면 TypeMismatch), 생성 접근기가 싣는 C++ enum의 native 키(`RuntimeFieldOps::valueNativeType`)가 그 레코드의 키와 같아야 한다(다르면 TypeMismatch). 등록된 클래스는 쓰는 enum을 의존으로 붙잡으므로 그 enum의 `Unregister`는 Busy다(lease가 아니므로 `Shutdown`을 막지 않는다). `Acquire`는 클래스만 돌려주고(enum ID면 TypeMismatch), enum은 `AcquireValueType(id, TypeKind::Enum)`으로 열어 `TypeLease::Enum()`으로 스키마를 읽는다. 클래스 lease의 `FieldEnum(fieldIndex)`는 그 필드가 가리키는 enum의 스키마다.

Enum 필드의 `FieldValue`는 `kind = ValueKind::Enum`, `signedInteger = 값`, `type = enum ID`다. 쓰기(등록된 `write`·`writeStaged`·등록 전 callback)는 `type`이 필드의 enum ID와 다르면 TypeMismatch, 선언되지 않은 값(범위 밖 포함)이면 InvalidArgument이며 값은 그대로다. 그래프 저장은 C++에서 `static_cast`로 넣은 선언되지 않은 값을 InvalidArgument로 거부하고, 복원은 기본 정책에서 저장된 선언되지 않은 값을 객체를 만들기 전에 InvalidArgument로 거부하고, `UndeclaredEnumPolicy::ReportAndDefault`면 `UndeclaredEnumValue`를 진단한 뒤 대체 값을 쓴다([Serialization](Serialization.ko.md) "필드 codec과 문맥").

### 리플렉션 struct(`ROCKS_STRUCT`)

```cpp
ROCKS_STRUCT(Version = 2)
struct BulkDataRef {
    ROCKS_PROPERTY() rocks::uint32 bulkId = 0;
    ROCKS_PROPERTY() rocks::Sha256Digest digest;
};

ROCKS_CLASS()
class TextureSource : public rocks::Object {
public:
    ROCKS_PROPERTY() BulkDataRef bulk;
};
```

`ROCKS_STRUCT(...)`은 `ROCKS_CLASS`와 같은 규칙으로 바로 뒤의 완전한 `class`/`struct` 정의 하나에 붙는다(한 레코드에 둘을 함께 쓰면 거부한다). 대상은 base가 없는 비템플릿 타입이고(상속은 RMC2301, 템플릿은 RMC2000), 모든 비정적 데이터 멤버는 `ROCKS_PROPERTY` 또는 `ROCKS_NATIVE`이며 `explicit S(Allocator&) noexcept` 또는 `S() noexcept` 생성자와 noexcept 소멸자가 있어야 한다(생성 `MakeRuntimeStructOps<S>`의 `static_assert`). private 멤버는 클래스처럼 `ROCKS_GENERATED_BODY()`를 둔다. 클래스 안에 중첩한 리플렉션 struct(와 클래스)는 public 구역이어야 한다(RMC2000 — 생성 코드가 바깥에서 이름을 쓴다). 지정자는 `Version=N`(없으면 1)과 `Binds="…"`(Core 값 타입 결합 — 아래 "Core 값 타입")이다. struct는 클래스의 base나 `ObjectRef<T>`·`SoftObjectRef<T>`의 대상이 될 수 없다(RMC2003). struct 멤버로 다른 struct를 담을 수 있고 중첩 깊이(1 + 멤버 struct 깊이의 최댓값)는 `MaxStructDepth` = 16 이하다(RMC2303). 순환은 RMC2302다 — 일반 값 멤버로는 생기지 않지만 Binds 거울이 자기가 묶는 실제 타입을 담으면 생기고, import한 descriptor에서도 검사한다. struct 타입과 필드도 identity 명세의 `type`·`field` 항목이다.

생성 형질: `ObjectRef` 멤버(Transient 포함)나 그런 struct 멤버를 담으면 `ContainsStrongReference`(참조 struct — 소유 클래스의 생성 Trace가 struct의 Trace를 거쳐 방문한다), `RocksRequiresWorldOnlyOwner` 형질 NATIVE 멤버나 그런 struct 멤버를 담으면 `RequiresWorldOnlyOwner`다. struct의 WorldOnly 소유자 형질은 오류가 아니라 전파되며 그 struct를 필드(상속 필드·NATIVE 멤버 포함)로 둔 WorldOnly 아닌 클래스가 RMC2206이다. NATIVE 멤버로 둔 참조 struct는 RMC2205다. 다른 모듈의 private struct, 같은 모듈 public 타입의 private struct 필드·NATIVE 멤버는 RMC2006이다.

생성 헤더는 `GeneratedType<S>`에 `ContainsStrongReference`·`RequiresWorldOnlyOwner`·`Depth`(constexpr), `Id()`, `Schema()`(base·클래스 플래그 없는 `TypeSchema`), `Trace(const S&, TraceVisitor&)`(모든 struct — 참조 없는 struct는 빈 본문), `Ops()`(`RuntimeStructOps` — 멤버마다 `detail::StructFieldOps<&S::m>`, 참조 struct만 `trace`), `Register`를 낸다. 형질과 깊이는 MetaCompiler 모델 값이고 직접 멤버 struct의 생성 값을 포함하는지 `static_assert`로 대조한다(import descriptor의 flags를 믿지 않는다). struct가 `Migrate`라는 이름의 멤버(메서드·함수 템플릿, 접근 무관)를 선언하면 MetaCompiler가 AST에서 그것을 보고 생성 `Ops()`가 `ops.migrate = &S::Migrate;`(타입 마이그레이션)를 낸다. 시그니처는 `static Result<Array<uint8>> Migrate(Allocator&, TypeId, uint32 from, uint32 to, Span<const uint8> record) noexcept`여야 하며 다르면(noexcept 없음·비정적 포함) 생성 코드가 컴파일되지 않는다. private·protected `Migrate`는 `ROCKS_GENERATED_BODY()`가 있어야 한다(없으면 RMC2000 — 생성 특수화가 friend로 주소를 얻는다). 클래스의 struct 필드는 `read`·`write`·`writeStaged` 없이 codec과 struct native 키(`valueNativeType`)만 싣는다(쓰기는 Unsupported — 멤버 단위 쓰기는 경로 접근 B6). 다른 헤더(같은 모듈 포함)나 import base의 상속 필드가 쓰는 struct는 그 생성 헤더를 include하고, 한 헤더 안의 특수화는 쓰는 struct(PROPERTY 멤버와 NATIVE 리플렉션 struct 멤버 — 생성 `static_assert`가 둘 다 쓴다)가 먼저 나온다. 모듈 집계 함수는 enum → struct(`ModuleRegistration::RegisterStructIfAbsent`, 같은 순위 — 1 + 이 모듈의 쓰는 struct 순위 최댓값 — 순·같은 순위는 이름 순) → 클래스 순으로 등록한다. 저장·복원·버전 정책은 [Serialization](Serialization.ko.md)의 "리플렉션 struct 값"이다.

### Core 값 타입(`CoreTypeBindings`, `Binds`)

Core 수학 타입은 MetaSchema·ObjectRuntime에 의존하지 않는 Core에 있어 annotation을 달 수 없으므로, Metadata 모듈 `CoreTypeBindings`(Core/Source/Runtime/CoreTypeBindings, 헤더만, Engine SDK 타깃에 포함)의 `CoreTypeBindings/CoreValueTypes.h`가 거울 선언으로 리플렉션한다. 대상: `Vector2·Vector3·Vector4<float|double>`(별칭 `Vec2`·`Vec2d`·`Vec3`·`Vec3d`·`Vec4`·`Vec4d`), `Quaternion<float|double>`(`Quat`·`Quatd`), `Rotator<float|double>`(yaw·pitch·roll 라디안), `Color`(uint8 RGBA), `LinearColor`(float RGBA), `IntVec2`·`IntVec3`(int32), `Transform`(`Vec3d translation`·`Quat rotation`·`Vec3 scale` — 다른 거울에 의존). Core 값 타입 필드를 리플렉션하는 모듈은 `CoreTypeBindings`에 의존한다(의존하지 않고 그런 필드를 쓰면 "unsupported field type" 진단이 의존을 더하라고 안내하고, 의존하는데 묶이지 않은 인스턴스 — 예: `Vector3<int>` — 를 쓰면 묶인 인스턴스를 알려 준다). 필드에는 실제 타입(별칭 포함)을 쓰고 거울 타입(`…Binding`)을 직접 쓰면 거부한다(RMC2000).

```cpp
ROCKS_STRUCT(Binds = "rocks::Vec3", Version = 1)
struct Vec3Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
};
```

- `Binds`는 `CoreTypeBindings` 모듈에서만 받는다(다른 모듈은 RMC2004 — 무관한 두 모듈이 같은 타입을 묶으면 메타데이터가 두 번 정의된다). 한 실제 타입은 거울 하나만 묶고(별칭으로 같은 타입을 두 번 묶어도 RMC2004), 리플렉션 타입을 묶을 수 없다(RMC2004). 철자는 전역 기준 완전 한정 이름(앞의 `::`는 있어도 없어도 된다 — 없으면 붙여 해석한다)과 템플릿 인자만 받는다(그 밖은 RMC2000).
- 철자는 문자열로 비교하지 않는다. MetaCompiler가 같은 합성 번역 단위 끝(전역 범위)에 거울마다 `using rocksBinds_N = ::<철자>;`와 완전한 class 타입인지 확인하는 `static_assert`를 붙여 Clang을 한 번 더 돌리고(`-ast-dump-filter`) 별칭의 desugar 철자를 정규 철자로 쓴다(`rocks::Vec3` → `rocks::Vector3<float>`). 이 실행의 오류는 그 거울 선언 위치의 RMC3000이다. 정규 철자가 descriptor `binds` 문법(한정 이름과 이름·음이 아닌 정수 템플릿 인자) 밖이면(예: 별칭으로 묶은 `Fixed<-3>`) 그 거울 위치의 RMC2000이다 — 의존 모듈이 descriptor를 읽지 못하게 되지 않도록 생성 모듈에서 막는다. 필드 형식은 이 정규 철자로 거울을 찾는다(이 모듈과 import 모두).
- 생성 코드는 거울이 아니라 실제 타입의 `GeneratedType`을 내고 멤버 thunk·Trace·생성·소멸·native 키가 모두 실제 타입을 쓴다(`ClassModel.NativeName`). 스키마 이름은 거울 이름이다. 생성 헤더의 비템플릿 함수 `VerifyBinds`가 실제 타입에 대해 멤버마다 `__is_same(decltype(Real::m), T)`(이름·형식)와 `__is_same(decltype(&Real::m), T (Real::*))`(base가 아니라 실제 타입에 직접 선언), 구조적 바인딩(`[[maybe_unused]] const auto& [m0, …] = value;`)과 위치별 형식(개수 — private 데이터 멤버·union·base 분할도 실패), `__is_standard_layout`과 연속 멤버의 `ROCKS_OFFSET_OF` 증가 사슬(선언 순서 — 같은 형식 멤버끼리 바뀐 순서 포함)을 확인한다 — 이름·형식·개수·순서가 다르면 컴파일되지 않는다. sizeof는 대조하지 않는다(꼬리 패딩의 숨은 멤버는 구조적 바인딩 개수가 잡는다). 생성·소멸 noexcept는 실제 타입에 건다. 거울에는 NATIVE 멤버와 `Migrate`를 두지 않는다(RMC2000).
- descriptor struct 항목의 `binds`가 정규 철자다(거울이 아니면 "", 앞에 `::`가 없다). import는 철자 문법을 검사하고(RMC1000) 모듈·중복·리플렉션 타입 결합을 RMC2004로 거부한다. 저장은 다른 struct와 같은 태그형 중첩 Record다(고정 배치를 쓰지 않는다).

`Category`와 `DisplayName`은 바이트 길이를 가진 UTF-8 데이터이며 중간 NUL(`\u0000`)도 유효하다. `Span<const char>`의 길이를 사용해야 하며 NUL 종료 문자열 함수로 읽으면 값이 잘린다. 등록 시 이 바이트를 깊게 복사한다. 생성 C++은 한글 등 읽을 수 있는 문자를 유지하고 인용부호·역슬래시·제어 문자를 escape한다. 제어 바이트는 정확히 세 자리 8진수로 표현하므로 뒤에 숫자가 와도 서로 합쳐지지 않는다.

## 생성물과 등록

생성 헤더는 `GeneratedType<T>`의 `Id`, `Schema`, `Ops`, `Register`를 제공한다. `Schema()`는 타입·필드 ID, 버전, 이름, 값 형식(`ValueType`), 기반 타입 ID와 참조 대상 타입 ID를 제공한다. `Ops()`는 실제 C++ 타입으로 검증되는 read/write/writeStaged 함수, 그래프 저장·복원 codec(encode/decode — `Object/GeneratedFieldCodec.h`)과 자동 Trace를 제공한다.

모듈마다 집계 헤더 `<M>/<M>.module.generated.h`가 생긴다. 이 헤더는 그 모듈의 public 생성 헤더를 모두 include하고 `rocks::generated::<M>::RegisterModuleTypes`를 제공한다. 이 함수는 registration마다 모듈을 한 번만 등록하고(의존 그래프의 여러 경로로 다시 불리면 바로 성공한다), import한 의존 모듈의 `RegisterModuleTypes`를 먼저 부른 뒤 이 모듈의 public 타입을 base 먼저 등록한다. 같은 ID가 같은 native 타입으로 이미 등록되어 있으면 새로 등록하지 않고 그 타입의 lease를 붙잡으므로, 여러 모듈이 같은 의존 모듈을 공유해도 한 번만 등록되고 공유 타입을 쓰는 쪽이 있는 동안에는 그 타입이 해제되지 않는다. 실패하면 그 호출의 기록을 역순으로 되돌리고 오류를 돌려준다. private 헤더가 있는 모듈은 `<M>/<M>.private.module.generated.h`의 `RegisterPrivateModuleTypes`가 public 타입(public 헤더가 없으면 의존 모듈)까지 등록한 뒤 private 타입을 등록한다. 이 헤더는 그 모듈의 private include에서만 보인다. 집계 헤더의 이름은 모두 `::rocks::`로 한정하므로 모듈 이름이 rocks의 이름을 가리지 않는다.

```cpp
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>

rocks::ModuleRegistration registration(types, allocator);   // types: TypeRegistry
if (auto registered = rocks::generated::ReflectionTestFixtures::RegisterModuleTypes(registration); !registered) {
    return registered;   // 이 호출의 기록(등록·lease)은 이미 되돌려졌다
}
// ... 종료할 때 (객체나 다른 기록의 lease가 남으면 Busy, 남은 기록을 보존하므로 다시 호출할 수 있다)
if (auto removed = registration.Unregister(); !removed) { ... }
```

`ModuleRegistration`은 들어온 모듈(`EnterModule`), 자기가 등록한 타입의 token, 이미 등록되어 있어 붙잡은 타입의 lease를 기록한다(`Size()`는 모두 센다). `Unregister()`·`UnregisterFrom(mark)`는 기록을 역순으로 되돌린다: lease는 놓고 등록한 타입은 해제한다(파생 타입이 base보다 먼저 빠진다). 해제할 타입에 객체나 다른 기록의 lease가 남아 있으면 Busy로 멈추고 남은 기록을 보존한다. 소멸자는 등록한 타입을 해제하지 않고(lease만 놓는다) 호출자가 `Unregister()`를 부른다. lease가 남아 있으면 `TypeRegistry::Shutdown`이 Busy이므로 모든 기록을 해제한 뒤 Shutdown한다. TypeRegistry와 allocator는 이 객체보다 오래 살아야 하며 등록·해제는 TypeRegistry 생성 스레드에서만 한다.

`MakeRuntimeTypeOps<T>`는 `noexcept T(Allocator&)`가 있으면 Registry allocator를 전달하고, 없으면 `noexcept T()`를 호출한다. 실패 가능한 초기화는 생성자 이후의 ConstructionContext 단계에서 처리한다. 자동 생성한 `Ops()`는 전체 지원 필드를 확인한 후 `completeFieldLayout = true`를 지정하고, 부수 효과 없이 ObjectRef를 방문하는 Trace에 `parallelTraceSafe = true`를 지정한다. 수동 Ops의 두 값은 기본적으로 false다. 실제 병렬 실행의 허용 여부는 GC의 단계·safepoint 규약에 달려 있다.

TypeRegistry는 스키마, 이름·속성 바이트와 RuntimeFieldOps 배열을 등록 시 깊게 복사한다. 원본 배열을 함수 지역 변수로 두어도 등록 이후의 배열 수명에 의존하지 않는다. 코드 포인터의 동적 모듈 수명은 `ModuleLease`가 유지한다. 등록 실패 시 기존 레코드는 유지되며 값으로 전달한 Ops의 ModuleLease는 정상 반환된다.

필드 ID의 중복, 지원하지 않는 형식 값, 스키마와 함수표의 불일치, 빠진 callback, 기반 타입의 미등록, 기반 native type 불일치, 상속 필드 표현의 불일치를 등록 단계에서 거부한다. 수동 함수 포인터가 실제로 올바른 C++ 필드를 처리하는지는 런타임에서 증명하지 못한다. 수동 메타데이터 제공자는 스키마·typed thunk·Trace의 일치를 지킬 책임이 있다.

## TypeLease와 상속 수명

기반 타입을 먼저 등록한다. 파생 레코드는 기반 레코드에 의존하므로, 외부 lease가 없어도 파생 타입이 남아 있으면 기반 타입 Unregister는 `Busy`를 반환한다. 파생 타입의 TypeLease가 있으면 파생 스키마와 그 기반 연쇄가 유지된다. 내부 기반 의존성은 외부 TypeLease 계수와 분리하여 Shutdown을 영구적으로 막는 순환 계수를 만들지 않는다.

Shutdown은 새 Acquire/Clone을 닫고 외부 lease가 있으면 `Busy`를 반환한다. 모든 lease를 반환한 뒤 재호출하면 파생 타입부터 기반 타입 순서로 정리한다. allocator·모듈 정리 callback은 Registry 잠금 밖에서 호출한다. 등록·해제·Shutdown은 생성 스레드 전용이며 Acquire와 서로 다른 lease의 계수 조작은 잠금으로 보호된다. 같은 lease 래퍼의 동시 변경은 허용하지 않는다.

`TypeLease::IsA`와 `TypeRegistry::IsA`는 스키마 ID 기준의 상속 관계 조회다. native 상속 판정은 `TypeLease::IsNativeA(NativeTypeKey<T>())`가 등록된 base 사슬을 따라 한다. 등록 시 각 레코드의 `nativeBaseType`이 base 레코드의 `nativeType`과 같은지 검증하고 단일 비가상 상속만 허용하므로, base 사슬은 실제 C++ 상속 사슬과 같다. 이 규칙으로 `Resolve<T>`·`TryPin<T>`·`NewObject<T>`·`SetReference`(필드 선언 타입)·리플렉션 필드 쓰기·객체 그래프 저장과 로드는 T 자신과 T의 파생 객체를 허용한다. Registry는 항상 `Object` 하위 객체 주소를 주고 `Borrow<T>::Get`·`ConstructionContext::Get`이 `static_cast<T*>`로 기반 하위 객체 위치를 보정한다. 자동 생성한 파생 필드 thunk는 정확한 파생 native type을 Resolve하고, 상속 멤버에 대한 typed member pointer로 접근한다.

## 필드 접근과 실패 보장

`FieldValue`는 이동 전용 소유 값이며 String도 자체 복제한다. kind에 대응하는 payload만 유효한 값으로 읽는다. 같은 FieldValue의 동시 변경이나 allocator callback에서 그 래퍼로 변경 재진입하는 동작은 금지한다. allocator는 값과 진행 중인 호출보다 오래 살아야 한다.

일반 read와 쓰기 가능 필드의 write는 ObjectAccessContext와 ObjectHandle을 받아 Registry에서 owner thread, context, generation, 논리 수명과 owner의 native type을 검사한다. 참조 필드 값은 선언 타입 또는 그 파생 객체여야 한다. raw object pointer나 StringView를 외부 반환값으로 유출하지 않는다. staged write는 ConstructionContext의 아직 게시되지 않은 객체를 검사한다.

- String read는 임시 소유 값에 복제하고, 할당 callback 이후 Borrow의 논리 유효성을 다시 확인한 뒤 output에 게시한다. OOM 또는 논리 파괴는 기존 output을 유지한다.
- String write는 대상 필드의 allocator로 replacement를 완성하고, 논리 유효성을 다시 검사한 후 할당·해제 없는 Swap으로 commit한다. OOM과 commit 전 논리 파괴는 기존 필드를 유지한다.
- Swap 이후 이전 문자열의 storage 반환 callback에서 객체를 논리 파괴해도 이미 완료한 commit은 성공이다. 일반 write의 Borrow는 이전 storage 정리까지 객체의 물리 수명을 보존한다. 이때 추가 객체 접근을 하지 않는다.
- 정수 폭 축소는 범위를 확인하고 `Overflow`를 반환한다. 타입이 다르면 `TypeMismatch`이며 필드는 변경하지 않는다.
- ObjectRef write는 `ObjectRegistry::SetReference` 또는 `ConstructionContext::SetReference`를 통과한다. owner/target 검증, typed target 규칙, GC barrier를 raw store로 우회하지 않는다.

등록된 `TypeLease::Ops()`의 모든 일반 write는 실제 대상 객체의 타입 lease를 조회하고 native 타입, 필드 ID와 값 형식을 검증한다. 대상 타입에 필드가 없으면 `NotFound`, native 타입·필드 형식·전달 값 형식이 다르면 `TypeMismatch`를 반환한다. 오류 우선순위는 context·수명, 타입·필드, 전달 값 종류, 읽기 전용 정책, 값 내용(정수 폭 `Overflow`, 소프트 참조의 null 규칙·경로 상한 `InvalidArgument`) 순서다. 따라서 같은 native 타입의 다른 TypeId 별명이나 별도 TypeRegistry의 객체도 실제 대상 schema의 필드 정책을 검사한다.

`ReadOnly`는 등록된 함수표의 일반 write에 적용되는 편집 정책이다. 원본 또는 대상 schema 중 하나라도 해당 필드를 ReadOnly로 선언하면 `Unsupported`를 반환한다. 미게시 객체에 대한 `writeStaged`는 초기화와 복원을 위해 허용한다. 등록된 write마다 타입 lease 조회와 필드 검색 비용이 발생하며, 안전한 정책 확인을 우선한다. `write`는 callable 값이며 기존 `.write(...)` 호출 구문을 유지한다. 이 값의 callback 코드 수명은 `TypeLease`가 보유하는 `ModuleLease`에 종속된다.
ObjectReference 필드는 등록 시점에 참조 대상 타입이 아직 등록되지 않을 수 있으므로 write 시점에 원본 및 대상 TypeRegistry에서 각각 선언 타입을 조회한다. 둘 다 구체 타입이면 native 타입이 같아야 하며, 서로 다른 TypeId가 같은 native 타입을 가리키는 별칭은 허용한다. 한쪽이 0 GUID이면 일반 Object로 취급하고 다른 쪽의 구체 타입 제약을 적용한다. 실제 참조가 비어 있지 않으면 두 구체 제약에 모두 맞아야 한다. 두 구체 선언이 서로 충돌하거나 타입을 조회하지 못하면 null 대입도 거부한다. 정상적인 자기 참조 선언의 null은 허용하며 부분적으로만 비어 있는 잘못된 핸들은 거부한다.

이 정책은 C++ 멤버에 대한 직접 접근이나 등록 전 `GeneratedType<T>::Ops()`의 원본 callback 호출을 차단하는 보안 경계가 아니다. 수동 코드 제공자는 원본 callback과 필드 접근의 사용 범위를 관리해야 한다.
`String::Swap`은 allocator와 storage 소유권도 함께 교환한다. 양쪽 allocator가 각 저장소의 최종 반환보다 오래 살아야 한다. 같은 String의 변경 재진입은 기존 String 계약대로 금지한다.

## 호스트 실행과 빌드 연결

다음 예시는 저장소 루트에서 실행한다.

```text
dotnet build Programs/Rocks.Tools.csproj
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll --module SampleGame --input Templates/Game/Foundation/Source/GameObject.h --include GameObject.h --output-root out/Generated/SampleGame --descriptor out/Metadata/SampleGame.rocksmeta.json --identity-manifest Templates/Game/Foundation/SampleGame.rocksids.json --compiler clang-cl
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll meta --root . --compiler clang-cl --metacompiler out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll --clang-arg=-DROCKS_BUILD_DEBUG=1
```

ToolTests `meta`는 시험마다 자기 임시 디렉터리에서 MetaCompiler·Clang 프로세스를 띄우므로 기본으로 CPU 수만큼 함께 돌린다(`--jobs N`으로 바꾼다). 결과 줄은 끝난 순서로, JSON 보고서는 이름 순서로 남는다. 프로세스 전역 상태(환경 변수 등)를 바꾸는 시험은 `exclusive`로 등록해 병렬 묶음이 끝난 뒤 하나씩 돈다. 새 meta 시험은 저장소 파일을 읽기만 하고 쓰기는 자기 임시 디렉터리에 한다.

MetaCompiler는 모듈 하나를 한 번에 생성한다. `--module`(모듈 이름), 짝을 이루는 `--input <헤더>`·`--include <철자>`(여러 번), private 헤더 표시 `--private <철자>`, 생성 루트 `--output-root`, descriptor `--descriptor <M>.rocksmeta.json`, 의존 모듈 descriptor `--import`(여러 번)를 받는다. 입력 헤더 전부를 include하는 번역 단위 하나를 Clang에 주므로 같은 모듈의 헤더 사이 base·참조가 풀린다. 생성 헤더는 `<생성 루트>/Public|Private/<철자의 .h를 .generated.h로 바꾼 경로>`에 쓴다. 위 예는 `out/Generated/SampleGame/Public/GameObject.generated.h`, 집계 헤더 `out/Generated/SampleGame/Public/SampleGame/SampleGame.module.generated.h`와 descriptor를 만든다. 실제 빌드에서는 빌드 도구가 이 인자를 모두 구성한다.

컴파일러는 C++23, 예외·RTTI 비활성화와 경고 오류 정책을 적용한다. Windows SDK 환경은 공통 NativeEnvironment가 준비한다. macOS에서 위 예시의 `clang-cl`은 `clang++`로 바꾼다. annotation은 Clang 기능이며 지원하지 않는 컴파일러에서 빈 매크로로 대체하지 않는다.

`--include-dir`을 지정하면 전달한 순서만 사용한다. 빌드는 native 컴파일과 같은 순서 — 소유 모듈의 public include(끝에 생성 Public 루트), private include(끝에 생성 Private 루트), 의존 모듈의 public include(각 모듈의 생성 Public 루트 포함) — 로 넘기며, 생성 루트는 이번 실행의 stage 루트로 바꿔 넘긴다. 지정하지 않은 단독 호출에서는 저장소의 공개 헤더 경로, 저장소 루트, 입력 헤더의 디렉터리를 탐색한다. 생성 헤더는 원본을 `#include <철자>`로 포함하므로 각 `--include` 철자는 이 검색 순서에서 실제로 `--input` 파일로 풀려야 한다. 앞선 디렉터리의 다른 헤더가 같은 철자를 가리면 MetaCompiler가 RMC1000으로 거부한다. 빌드는 그 전에 public 리플렉션 헤더를 쓰는 타깃 안 모든 모듈의 검색 순서로 이를 확인하고, 생성 철자(`.generated.h`·집계 헤더)가 생성 루트보다 앞선 디렉터리의 파일에 가려지는지도 본다(RMC1001). 생성물 검증 컴파일은 검색 순서에서 출력 루트가 있던 자리에 검증용 임시 루트를 넣어 소비자와 같은 순서로 푼다.

반복 가능한 `--clang-arg`는 현재 네이티브 Debug/Release/ASan 프로필, 붙임형 `-DNAME=value` 및 clang-cl의 `/DNAME=value`, macOS에서 xcrun이 선택한 `-isysroot` 경로와 최소 버전만 허용한다. 경고 억제, 임의 검색 경로, 강제 include, response file과 그 밖의 미지원 인자는 거부한다. 옵션 확장은 네이티브 컴파일과 AST에 적용되는 조건이 같은지 검토하고 양성·음성 회귀를 추가한 후 수행한다.

C++ 구문은 원문 정규식이 아닌 실제 Clang JSON AST에서 읽는다. 선언의 annotation 인자, 접근 지정, 기반 관계와 컴파일러가 확정한 필드 타입을 사용한다. `--include`는 출력 헤더에 넣을 논리 경로이며 절대 입력 경로는 생성물에 포함하지 않는다.

생성 형식은 `ROCKS_CLANG_FORMAT` 환경변수를 우선 사용하고 clang-format 21.1.6을 요구한다. 같은 입력·타깃 문맥이면 같은 UTF-8/LF 바이트를 생성한다. 생성물은 출력 루트 옆의 고유 임시 디렉터리(`.rocks-meta-validation-*`)에 두고, 생성 헤더와 집계 헤더마다 따로 실제 Clang으로 검사한 뒤 게시한다. 정상 종료와 관리된 실패에서 임시 디렉터리를 정리한다. 기존 출력과 같으면 다시 쓰지 않아 timestamp를 보존한다. 변경된 결과는 같은 디렉터리의 고유 임시 파일에 완성한 뒤 교체한다. 단독 실행은 입력에서 빠진 헤더의 이전 생성물을 지우지 않는다(지우기는 빌드의 게시 트랜잭션이 맡는다). `--check`는 모든 생성 헤더·집계 헤더·descriptor를 기존 출력과 비교하고 생성 루트에 남은 잉여 파일도 stale로 거부하며, 출력을 수정하지 않는다. 서로 다른 생성 설정이 같은 출력을 공유하는 빌드 그래프는 금지하며, 고유 임시 파일 사용이 그런 논리 충돌을 해결하는 것은 아니다.

C# 빌드 도구는 타깃에서 도달하는 모듈 가운데 `Metadata`가 있는 모듈을 의존 순서로 하나씩 생성한다. 생성 디렉터리 `<out>/Generated/<M>/Public`·`Private`는 각각 그 모듈의 public·private include 디렉터리가 되어 의존 모듈로 전파된다. 생성은 `<out>/.metadata-stage/`에 하고, 의존 모듈은 stage의 descriptor와 생성 루트를 본다(이전 빌드의 게시본을 보지 않는다). 모든 모듈의 생성·`ids verify`·`ids verify-set`이 성공하면 생성 헤더·descriptor(`<out>/Metadata/<M>.rocksmeta.json`)와 입력에서 빠진 헤더의 이전 생성물 삭제를 한 트랜잭션으로 게시한다. 게시 중 실패하면 교체와 삭제를 모두 되돌리며, 실행 동안 명세가 바뀌었으면 게시하지 않는다. Core-only 구성은 이 생성 작업을 요구하지 않는다. 생성 헤더는 public include 헤더 검사에서 일반 엔진 헤더와 같은 경고 오류 기준으로 단독 컴파일된다.

## 진단과 수정

진단은 가능한 원본 파일과 줄, 오류 코드, 원인, `hint`를 함께 출력한다. 예를 들어 지원하지 않는 `PROPERTY` 지정자는 `RMC2000`으로 거부한다. `RMC1000`은 잘못된 명령·경로·지원하지 않는 컴파일 옵션, `RMC1001`은 리플렉션 헤더 철자를 가리는 다른 헤더, `RMC2001`은 처리할 수 없는 Clang AST 형태, `RMC2003`은 모듈 입력에도 import한 descriptor에도 없는 리플렉션 선언·base·참조 대상, `RMC2004`는 같은 모듈 또는 같은 타입을 두 번 import하거나, 자기 descriptor를 import하거나, import한 타입 이름을 이 모듈이 다시 정의한 경우, `RMC2005`는 대소문자만 다르거나 겹치는 생성 철자와 예약된 `.module.h`·`.generated.h` 이름, `RMC2006`은 private 타입을 public 타입의 base로 쓰거나 다른 모듈에서 쓴 경우, `RMC2100`은 명세 누락·변경과 inventory 불일치, `RMC2101`은 명세 사이 ID·이름 중복, `RMC2102`는 Metadata가 없는 모듈의 활성 명세, `RMC3000`은 Clang 입력 또는 생성 코드의 계약 오류, `RMC3100`은 포매터 오류, `RMC4000`은 `--check`의 stale 출력을 나타낸다. 실패 시 기존 생성물은 유지하며 빌드는 실패로 판정한다.

## 현재 한계와 검증 범위

모듈의 리플렉션 헤더는 한 번역 단위로 처리하고, 그 모듈 입력 헤더에 물리적으로 있는 선언만 생성 대상으로 모은다. include로 들어온 다른 모듈의 리플렉션 선언은 import한 descriptor에 같은 이름·같은 헤더 파일로 있어야 하며 다시 생성하지 않는다. compile_commands 기반 정확한 타깃 문맥 복원, 원격·로컬 생성 캐시, depfile 기반 최소 재생성은 아직 지원하지 않는다. 여러 타깃의 define·include 차이는 호출 측에서 정확히 전달해야 한다. 컴파일러나 타깃 문맥이 달라져도 항상 같은 산출물을 보장한다는 의미는 아니다.

런타임 회귀는 scalar/Guid/UTF-8 소유 값, 상속 관계와 native cast 분리, 잘못된 context·thread, staged write와 참조 barrier, typed target 거부, descriptor 함수표의 깊은 복사, String OOM·논리 파괴 재진입, Swap의 allocator 소유권, 등록 OOM 후 기반 의존성 원복을 검사한다. 계수·allocator ledger가 종료 후 0으로 돌아오는지도 확인한다.

C# 하네스는 실제 Clang을 호출하여 자동 ID, 잘못된 명세·중복·누락, 빠진 annotation, 미지원 필드·상속·Trace, 컴파일 오류와 경고 강등 옵션을 검사한다. 오류가 기존 출력 파일을 훼손하지 않는지, 같은 입력의 바이트·timestamp가 유지되는지, `--check`가 변경 없이 stale 출력과 잉여 생성물을 거부하는지도 검사한다. 모듈 모드는 여러 헤더·집계 헤더·descriptor 생성, 의존 모듈 import(상속 필드·참조 대상 ID), private 헤더 분리와 private 헤더만 있는 모듈의 의존 등록, 같은 이름·다른 파일의 선언, 자기 descriptor import·import 타입 재정의, descriptor의 엄격한 읽기, RMC2003·2004·2005·2006을 검사한다. identity 명령은 `ids move`의 ID·별칭 보존·중첩 타입·재실행, `ids verify-set`의 ID·이름 중복, ID 기준으로 합친 merge-check와 명세를 옮긴 Git 병합을 검사한다. 빌드 통합 시험은 MetadataLayout의 가림(RMC1001)·이중 소유·생성 철자 충돌·잘못된 선언·고아와 예약 명세·타깃 범위, 게시 트랜잭션의 삭제·되돌리기·백업 위치를 검사한다. 설치형 SDK 시험은 SDK descriptor import와, 메타데이터 모듈 두 개(라이브러리와 실행 파일)가 있는 게임에서 stage import, 상위 모듈 실패 시 미게시, Metadata에서 뺀 헤더의 생성물 삭제를 검사한다. 런타임 시험은 공유 모듈의 한 번 등록과 lease 보유, Busy에서 역순 해제 중단과 재시도, 다른 native 타입의 같은 ID 거부, 성공 실행에서 잰 모든 할당 실패 지점의 되돌리기를 검사한다.

이전 구현은 Windows 로컬 Debug·Release·ASan과 [Windows/macOS native CI](https://github.com/maxidea1024/rocks/actions/runs/42775786556)에서 생성 코드 및 실제 Clang 회귀 실행을 확인하였다. 최종 수치·산출물 hash는 [5차 검증 기록](../Reviews/ImplementationRound5.FoundationValidation.ko.md)을 따른다. macOS는 ARM64 Debug·ASan 범위이며 Intel Mac과 TSan은 미검증이다.

## 모듈 메타데이터

리플렉션 헤더는 그 헤더를 소유한 모듈의 규칙이 `Metadata`로 선언한다. 철자는 모듈의 public 또는 private include 디렉터리 기준이다. public 헤더의 타입은 의존 모듈이 쓸 수 있고, private 헤더의 타입은 그 모듈 안에서만 쓴다.

```csharp
public sealed class ReflectionTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ReflectionTestFixtures";

    protected override string[] RequiredModules => ["ObjectRuntime"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata =>
        ["ReflectionTestFixtures/ReflectionFixture.h", "ReflectionTestFixtures/AutoIdentityFixture.h"];
}
```

생성 헤더는 원본 철자의 `.h`를 `.generated.h`로 바꿔 include한다(`#include <ReflectionTestFixtures/ReflectionFixture.generated.h>`). 명세는 모듈 폴더의 `ReflectionTestFixtures.rocksids.json`이다. `ModuleRules`를 직접 상속하는 게임 모듈은 `NativeModuleInput`에 `Metadata`와 함께 명세가 있는 폴더 `ModuleDirectory`(프로젝트 상대 경로)를 적는다(`Templates/Game/Foundation/Game.Build.cs`).

빌드는 다음을 거부한다: `Metadata`가 있는데 명세가 없는 모듈(RMC2100), `Metadata`가 없는데 활성 항목이 있는 명세(RMC2102 — 은퇴 항목만 남은 명세는 ID 예약이므로 허용), 한 헤더를 두 모듈이나 한 모듈에서 두 번 선언, 모듈 include 디렉터리 밖이거나 없는 헤더, 대소문자만 다른 생성 철자(RMC2005). 등록하지 않은 헤더의 `ROCKS_CLASS` 선언은 일반 C++ 컴파일만으로 누락이 탐지된다고 보장하지 않는다. 컴파일 단위와 생성 선언의 coverage를 강제하는 기능은 후속 구현 항목이다.