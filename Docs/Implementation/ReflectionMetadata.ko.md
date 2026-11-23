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

현재 `TypeSchema`, `RuntimeTypeOps`, `TypeRegistry`, `TypeLease`는 등록된 타입에 관한 정보, 추상 여부·상속 메타데이터 및 모듈 수명 보호의 기반을 제공한다. 등록된 `TypeId`를 쓰는 `ObjectRegistry::NewObject<Object>(access, typeId)`는 런타임 타입을 지정하는 낮은 수준의 객체 생성 경로다. 이 경로를 타입 제한 클래스 참조나 자동 에디터 선택, 클래스 기본값 레코드([RuntimeClassMetadata](../Design/RuntimeClassMetadata.ko.md) '클래스 기본값 결정' — A5)와 동일한 완성 기능으로 해석하지 않는다.

클래스 자체를 데이터로 전달·저장하고 기반 타입 제한, 동적 생성, 기본값·필드·함수와 에디터·에셋 연결을 지원하는 기능은 [런타임 클래스 메타데이터 설계](../Design/RuntimeClassMetadata.ko.md)의 후속 목표다. 실제 C++ 구현·사용 예제·플랫폼별 테스트가 완료된 부분만 이 문서의 현재 상태로 기록한다.

## 선언과 지원 형식

`ROCKS_CLASS(...)`를 `class`/`struct` 앞에, `ROCKS_PROPERTY(...)` 또는 `ROCKS_NATIVE()`를 필드 앞에 직접 작성한다. 선언에 안정 ID를 넣지 않는다(스키마 버전은 `Version=N` 지정자로 정한다). 매크로와 타입 사이에는 공백과 주석만 허용하고 전처리 지시문·별도 선언·wrapper 매크로는 거부한다. 조건부 선언은 마커와 클래스를 같은 `#if` 블록 안에 둔다.

리플렉션 헤더를 가진 모듈은 모듈 폴더에 identity 명세 `<ModuleName>.rocksids.json`을 둔다. 명세는 그 모듈 타입과 필드의 무작위 안정 ID, 이전 이름과 폐기 이력을 보관한다. 새로운 선언의 ID 발급과 이름 변경·폐기는 `ids add`, `ids rename`, `ids retire`를 사용하는 명시적인 저작 작업이다. 타입을 다른 모듈로 옮길 때는 `ids move --from <명세> --to <명세> --type <이름>`이 타입과 그 필드 항목을 ID 그대로 옮긴다. 대상에 먼저 붙이고 원본에서 떼므로 중간에 멈추면 같은 명령을 다시 실행한다. 일반 빌드는 명세를 읽기만 하고 누락 또는 중복을 거부한다. 모듈마다 생성 결과의 inventory로 활성 명세의 누락과 중복을 검사하고(`ids verify`), 타깃에서 도달하는 모듈의 모든 명세(은퇴 항목만 남은 예약 명세와 SDK가 동봉한 명세 포함) 사이에 같은 ID나 같은 종류의 같은 이름·별칭이 있으면 거부한다(`ids verify-set`, RMC2101 — 한 ID·한 이름은 한 명세에만 있다). 이 검사가 모두 끝나야 생성물을 게시한다. 리플렉션 헤더는 정확히 한 모듈이 소유한다. 미등록 헤더의 자동 탐색은 아직 없다 — 대신 엔진 저장소(템플릿 포함) 소스에서는 Policy가 어느 모듈 `Metadata`에도 없는 파일의 `ROCKS_PROPERTY`·`ROCKS_NATIVE`를 거부한다(아래 "native 클래스" — 저장소 밖 게임 프로젝트에는 아직 이 검사가 없다).

템플릿에서 새 프로젝트를 만들 때(`new-project`)는 `ids reissue --manifest <명세>`가 활성 항목에 새 무작위 ID를 주고 aliases와 은퇴 항목을 지운다. 같은 템플릿에서 만든 프로젝트끼리 타입 ID가 겹쳐 `ids verify-set`에 걸리지 않게 한다([Visual Studio 프로젝트 파일](../Design/ProjectFiles.ko.md) 11.4절).

이력 검사(`ids merge-check`, `Build/CheckIdentityMerges.ps1`)는 리비전마다 저장소의 모든 명세를 ID 기준으로 합쳐 비교한다. 따라서 명세를 옮기거나 타입을 다른 명세로 옮겨도 ID가 사라진 것으로 보지 않으며, ID 삭제·재사용과 예약 이름 재사용은 계속 거부한다. 합치기 전에 한 리비전 안의 두 명세에 같은 ID나 이름이 있는지 보고, 있으면 측면과 파일 경로를 함께 보고하고 실패한다(빌드의 `ids verify-set`과 같은 규칙).

저장 레코드에는 그 타입의 스키마 버전이 기록된다. 버전은 `ROCKS_CLASS(Version=N)`(없으면 1)이며 영속 필드의 의미·형식이 바뀌면 올리고 마이그레이션을 등록한다. base의 버전은 파생 Record에 기록되지 않으므로 base의 영속 배치를 바꾸면 파생 타입들도 버전을 올린다(MetaCompiler는 강제하지 않는다). 사용자 정의 스키마 변경의 자동 판정과 마이그레이션 생성은 아직 구현되지 않았다. 이름 변경 시 명세의 `ids rename`으로 ID를 유지하며, 다른 ID를 새로 발급해 이전 필드를 대체하면 활성 이전 선언이 누락되어 빌드가 실패한다.

| 필드 형식 | 런타임 표현 |
| --- | --- |
| `bool` | `ValueKind::Bool` |
| `uint8`, `uint16`, `uint32`, `uint64` | 폭에 대응하는 부호 없는 정수 |
| `int8`, `int16`, `int32`, `int64` | 폭에 대응하는 부호 있는 정수(`ValueKind::Int8`·`Int16`·`Int32`·`Int64`). 저장 wire는 폭과 무관하게 I64이고 로드·쓰기는 폭 밖 값을 Overflow로 거부한다. int8·int16은 리플렉션 B9a2이고 Clang 철자는 `signed char`·`short`다. 평범한 `char`는 부호가 구현 정의라 거부한다(RMC2000, `int8`·`uint8` 안내) |
| `float`, `double` | `Float32`, `Float64` |
| `Guid` | 두 개의 64비트 정수로 구성된 값 |
| `String` | 소유한 유효 UTF-8 문자열; 길이는 바이트 수 |
| `Name` | `ValueKind::Name`(리플렉션 B9c2); 실행 중 pool 토큰(`Core/Text/Name.h`). 저장 wire는 표시 문자열 String(번호 접미 포함, None은 빈 문자열 — UTF-8·NFC·NUL 없음·번호 접미를 뗀 base 1,023 UTF-16 단위 이하(UE 5.8과 같다), `Object/NameField.h`). 토큰은 Registry의 `RegistryOptions::names` pool 것이어야 하고(다르면 WrongContext, pool이 없으면 None만), 복원은 1단계가 표시 문자열을 검사·기록하고 2a가 그 pool로 intern한다. Required는 받지 않는다(RMC2202). 생성자 기본값은 None뿐이다 |
| `ObjectRef<T>` | `ObjectHandle`을 담은 참조 값; 실제 변경은 Registry barrier 경유 |
| `WeakObjectRef<T>` | `ValueKind::WeakObjectReference`(리플렉션 B9d1, `FieldKind::Value`); GC가 추적하지 않는 handle 값(`Object/ObjectRefs.h` — 대상이 죽으면 TryPin이 실패한다). `value.type`이 기대 타입(Object면 0)이고 대상은 리플렉션 클래스여야 한다(RMC2003). 쓰기는 같은 Registry의 생존 객체·기대 타입의 파생만 받고(null 허용) 쓰기 장벽·관찰자 기록이 없다. 저장 wire는 강한 참조와 같은 graph v2 U32이고 같은 그래프 안 노드만 보존한다(밖은 정책별 null + 진단 값 2, 죽은 대상은 진단 없이 null — `Docs/Design/ReflectionExtensions.ko.md` 5.1절). 생성 Trace는 방문하지 않는다. Required는 받지 않는다(RMC2202) |
| `ClassRef<Base>` | `ValueKind::ClassReference`(리플렉션 B9d2, `FieldKind::Value`); 클래스 TypeId 하나(`Object/ClassRef.h`, 0 = null — UE `TSubclassOf` 대응). `value.type`이 Base 클래스 ID(필수 — Base는 리플렉션 클래스여야 하고 `rocks::Object` 자체·struct·enum은 RMC2003). Base는 의존으로 붙잡지 않으므로 `ClassRef<Self>`와 아직 등록되지 않은 파생 클래스를 Base로 둘 수 있고, 생성 코드는 Base의 생성 헤더를 include하지 않는다(판정은 `NativeTypeKey<Base>`). 쓰기·저장·복원이 등록된 클래스이고 Base의 파생인지 본다(미등록 NotFound, 그 밖 InvalidArgument, 추상 클래스 허용 — 복원의 미등록 클래스는 decode 정책 `FieldDecodePolicy::unknownClass`). 저장 wire는 Guid. Required를 받는다(non-null) |
| `AssetId` | `ValueKind::AssetId`; 에셋 ID(`Object/AssetId.h`, Guid, 0 = null). 저장 wire는 Guid |
| `Sha256Digest` | `ValueKind::Digest256`; 32 byte 해시(`Core/Encoding/Sha256.h`). 저장 wire는 정확히 32 byte의 Bytes |
| `SoftObjectRef<T>` | `ValueKind::SoftObjectReference`; 에셋 ID와 패키지 안 객체 경로(`Object/SoftObjectRef.h`). GC 간선이 아니며 `value.type`이 기대 타입(Object면 0). 저장 wire는 `SoftObjectReference`(12) |
| `ROCKS_ENUM` enum class | `ValueKind::Enum`; `value.type`이 enum ID. 값은 선언된 값(Flags enum은 선언 값의 OR)만 유효하다. 저장 wire는 I64 (아래 "리플렉션 enum") |
| `ROCKS_STRUCT` struct | `ValueKind::Struct`(`FieldKind::Value`); `value.type`이 struct ID. 저장 wire는 중첩 Record(10) (아래 "리플렉션 struct") |
| `Array<T>`(T: 스칼라·`Guid`·`String`·`Name`·`AssetId`·`Sha256Digest`·enum·`SoftObjectRef`·`WeakObjectRef`·`ClassRef`·참조 없는 struct·Core 값 타입) | `ValueKind::Array`(`FieldKind::Value`); `element`가 T의 형식. wire는 단일 값 원소 `ValueArray`(13), struct 원소 `RecordList`(11) (아래 "리플렉션 배열") |
| `ObjectRefArray<T>`(원소 null 허용) | `ValueKind::Array`(`FieldKind::ObjectReference`); `element = {ObjectReference, T의 ID}`. wire는 참조 원소의 `ValueArray` |
| `StructRefArray<S>`(S: 참조 struct) | `ValueKind::Array`(`FieldKind::Value`); `element = {Struct, S의 ID}`. wire는 `RecordList` |
| `OrderedMap<K, V>`(K: 정수·bool·enum·`Guid`·`AssetId`·`Sha256Digest`·`String`·`Name`·`SoftObjectRef`·`ClassRef`, V: 단일 값(강한 참조 제외)·참조 없는 struct — 리플렉션 B9f2) | `ValueKind::Map`(`FieldKind::Value`); `key`가 K, `element`가 V의 형식, `maxElements` 상한. wire는 `Map`(16, 삽입 순서) |
| `OrderedSet<T>`(T: Map 키와 같은 종류 — 리플렉션 B9f2) | `ValueKind::Set`(`FieldKind::Value`); `element`가 T의 형식, `maxElements` 상한. wire는 `Set`(15, 삽입 순서) |
| `ObjectRefMap<K, T>`(K: Map 키와 같은 종류, 값은 T(기본 Object)의 강한 참조 — null 허용, Registry 컨테이너 — 리플렉션 B9f4) | `ValueKind::Map`(`FieldKind::ObjectReference`); `key`가 K, `element = {ObjectReference, T의 ID}`, `maxElements` 상한. wire는 `Map`(16, 값은 문맥 참조 wire). 편집은 ObjectRegistry `SetMapReference`·`EraseMapReference`·`ClearMapReferences`, 생성 트랜잭션은 `ConstructionContext::AssignMapReferences`([리플렉션 보강](../Design/ReflectionExtensions.ko.md) 7.4절) |

스키마의 값 형식은 `FieldSchema::value`(`ValueType { ValueKind kind; TypeId type; }`)다. `type`은 강한 참조와 소프트 참조의 기대 대상 타입(0 = Object)과 Enum의 enum ID·Struct의 struct ID(0이면 안 된다)이며 그 밖의 종류에서는 0이어야 한다. `element`·`maxElements`는 배열 필드용이다: 배열 필드는 `value = {Array, 0}`, `element`가 원소 형식(스칼라·Guid·Utf8String·ObjectReference·Enum·Struct·AssetId·SoftObjectReference·Digest256 — 원소 대상 ID는 `element.type`), `maxElements`가 1 이상 `MaxArrayElements`(1,048,576) 이하의 필드별 상한이고, 배열이 아닌 필드는 Opaque·0이어야 한다. TypeRegistry·codec·그래프 저장과 복원은 배열 필드를 받는다(B5b — 컨테이너 `Array<T>`·`ObjectRefArray<T>`·`StructRefArray<S>`와 wire는 [Serialization](Serialization.ko.md)의 "리플렉션 배열 값"). `TypeLease::FieldEnum`·`FieldStruct`는 배열 필드에서 원소의 enum·struct를 준다. MetaCompiler는 세 컨테이너의 PROPERTY 필드를 생성한다(B5c — 아래 "리플렉션 배열").

값 종류의 성질(필드·struct 멤버·Array 원소 가능 여부, Required, 대상 타입·대상 ID 필수, 컨테이너, 통째 FieldValue와 뒤 단계의 정적 배열·Map·Set·Optional·InstancedStruct 자리)은 성질 표 하나로 판정한다(리플렉션 B9a1 — [ReflectionExtensions](../Design/ReflectionExtensions.ko.md) 1.3절). 런타임 표는 `Meta/TypeSchema.h`의 `ValueKindProperties`와 질의 함수(`ValueKindCanBeArrayElement`·`ValueKindAcceptsRequired`·`ValueKindHasTargetType`·`ValueKindIsAggregate` 등 — 필드 하나의 aggregate 판정은 내부에 따라 달라지는 Optional 때문에 `FieldIsAggregate(const FieldSchema&)`, 리플렉션 B9e)이고 TypeRegistry 등록·쓰기 검증·경로·그래프 `ValidateLayout`·생성 codec이 이것을 쓴다(종류 값의 숫자 범위로 판정하지 않는다). MetaCompiler는 같은 표를 `ValueKindTable`(C#)로 갖고 스키마 검증과 descriptor 읽기의 종류 검사에 쓴다. 두 표의 일치는 `ValueKindTable.ParityHeader`가 낸 static_assert 대조 헤더를 ToolTests(`Meta.ValueKindTable*`)가 컴파일해 칸마다 양방향으로 확인한다. 새 종류를 더할 때는 `ValueKind` 끝, `ValueKindCount`, 두 표의 행을 함께 고친다.

`SoftObjectRef<T>`의 규칙: null은 asset이 0이고 경로가 빈 것이다(asset만 있고 경로가 비면 그 에셋의 대표 객체다). asset이 0인데 경로가 있거나, 경로가 4 KiB(`MaxObjectPathBytes`)를 넘거나 UTF-8이 아니면 InvalidArgument다. 리플렉션 쓰기(`write`·`writeStaged`·등록 전 callback), 그래프 저장(직접 대입한 값도 다시 검사)과 로드가 같은 규칙을 검사한다. 경로 문법(Outer 이름을 `.`로 잇는다)과 기대 타입은 대상을 찾을 때(A0-O·A1) 검사하며 쓰기·저장·로드는 검사하지 않는다. 이동 전용이고 이동하면 원본은 null이 된다. 이동 대입은 원본의 경로 allocator를 넘겨받으므로 객체 필드에 값을 넣을 때는 `TryAssign(other)`(받는 쪽 allocator 유지)를 쓰고, 복제는 실패할 수 있는 `TryClone(allocator)`다. 쓰는 클래스에는 `noexcept T(Allocator&)` 생성자가 필요하다(String 필드와 같다). `AssetId`의 문자열 표기와 해석은 36자 소문자 하이픈 형식 하나뿐이다(`TryParse`는 대문자·32자·중괄호를 거부한다).

현재 생성 대상은 이름 있는 비템플릿 타입이며, 모든 비정적 필드는 `ROCKS_PROPERTY`(영속·리플렉션 필드) 또는 `ROCKS_NATIVE`(비영속 런타임 멤버) 중 하나를 단다(둘 다면 RMC2204). 모든 리플렉션 클래스·struct는 정의 안에 `ROCKS_GENERATED_BODY()`를 직접 한 번 둔다(없으면 RMC2211 — 다음 문단). private/protected 필드 접근도 이 매크로가 연다. 리플렉션하지 않은 클래스, 다른 매크로로 감싼 호출, 수동 작성한 내부 annotation과 중복 선언은 거부한다. PROPERTY 필드에서 raw pointer, 리플렉션 배열 컨테이너(`Array`·`ObjectRefArray`·`StructRefArray`) 밖의 컨테이너, 리플렉션하지 않은 enum, const/volatile 필드는 거부한다(NATIVE 멤버는 강한 객체 참조만 아니면 이런 타입을 둘 수 있다). PROPERTY bitfield는 폭 1의 부호 없는 정수(uint8·uint16·uint32·uint64)만 Bool 필드로 받고(생성 접근자 `GeneratedType<Owner>::Get_<name>`·`Set_<name>`으로 읽고 쓴다 — `bool b : 1`·폭 ≠ 1·부호 있는 정수·enum·Binds 미러 안 bitfield는 RMC2207), NATIVE bitfield는 정수·bool이면 받는다(리플렉션 B9m — ReflectionExtensions 14절). 이름 없는 bitfield와 annotation을 붙인 static 필드는 거부한다. static 보조 데이터는 annotation 없이 둘 수 있다. 수동 `Trace`와 자동 추적을 섞어 완전한 필드 레이아웃으로 오인하지 않도록, 생성 대상의 직접 선언된 `Trace`는 거부한다. [리플렉션 B9](../Design/ReflectionExtensions.ko.md)가 `Name`·`WeakObjectRef<T>`·`ClassRef<Base>`·`Optional<T>`·`OrderedMap`·`OrderedSet`·`ObjectRefMap`·정적 배열 `T x[N]`·`InstancedStruct`·`Text` 필드, 폭 1 부호 없는 정수 bitfield(생성 접근자 — `bool b : 1`은 거부), NATIVE bitfield를 더한다(지원 형식 표는 ReflectionExtensions 1.3절, 진단은 15절 — 정수 폭 int8·int16은 B9a2에서 더했다, 위 표). `Optional<T>`는 B9e에서 더했다: 클래스 필드·struct 멤버에만 두고(배열 원소 위치는 RMC2304), 내부 T는 단일 값·`String`·`Name`·`AssetId`·`Sha256Digest`·리플렉션 enum·강한 참조 없는 리플렉션 struct이며(중첩 Optional·배열·참조·참조 struct·cv·지원하지 않는 형식은 RMC2306 — 강한 참조·보통의 참조 struct 내부는 Core `Optional`의 noexcept 이동 static_assert로 Clang이 먼저 실패하고 그 진단을 멤버 위치의 RMC2306(NATIVE면 RMC2205, 둘 다면 RMC2204)으로 바꾼다 — `Array`와 같은 변환), Required를 받지 않는다(RMC2202). 스키마는 `value = {Optional, 0}`·`element` = 내부 형식·`maxElements` 0이고 descriptor도 같은 모양(`element`)이다. 생성 함수표는 단일 값 내부면 read·write·writeStaged(FieldValue `kind` = 내부 종류, `present` = set 여부), 값 struct 내부면 codec과 경로 thunk(aggregate)이고 struct 순환 DFS(RMC2302)·깊이(RMC2303)는 Optional<struct> 간선도 센다. `OrderedMap<K, V>`·`OrderedSet<T>`는 B9f2에서 더했다: 클래스 필드·struct 멤버에만 두고(배열 원소 위치는 RMC2304, Optional 내부는 RMC2306), 키·Set 원소는 정수·bool·리플렉션 enum·`Guid`·`AssetId`·`Sha256Digest`·`String`·`Name`·`SoftObjectRef`·`ClassRef`, Map 값은 단일 값(강한 참조 제외 — `ObjectRefMap`)과 강한 참조 없는 리플렉션 struct다(위반·직접 중첩·키·값·원소의 Optional·기본이 아닌 해시·같음 정책·`HashMap`·`HashSet` PROPERTY는 RMC2305). 스키마는 Map이 `value = {Map, 0}`·`element` = 값 형식·`maxElements`·`key` = 키 형식, Set이 `value = {Set, 0}`·`element` = 원소 형식·`maxElements`이고 descriptor 필드에는 `key` 칸이 늘 있다(Map이 아니면 null). MaxElements는 배열·Map·Set에 쓰고, Required는 값·원소 종류(Soft·ClassRef)로 판정한다(키에는 없다). 생성 함수표는 codec·값 native 키·키 enum native 키(`keyNativeType`)와 경로 thunk(대상 종류 = 값·원소 종류 — key 단계 경로 연산, B9f3에서 더했다)다. 정적 배열 `T name[N]`은 B9g에서 더했다: 클래스 필드·struct 멤버에 두고 원소는 단일 값·`String`·`Name`·`ObjectRef`·`WeakObjectRef`·`SoftObjectRef`·`ClassRef`·리플렉션 enum·리플렉션 struct(참조 struct 포함 — Registry 컨테이너를 담은 struct는 아니다)이며, N은 1..65,535다(다차원·N 범위 밖·원소의 컨테이너·Optional·cv·지원하지 않는 형식·Registry 컨테이너를 담은 원소 struct는 RMC2307). 원소 형식은 Clang이 배열 원소를 쓰인 그대로 출력하므로 두 번째 Clang 실행(`StaticArrayProbe` — 필드마다 `decltype` 탐침 변수)으로 정규 철자를 얻는다. 스키마는 `value = {StaticArray, 0}`·`element` = 원소 형식·`maxElements` = N이고 ObjectRef 원소면 `FieldKind::ObjectReference`다. MaxElements는 쓸 수 없고(RMC2202) Required는 원소 종류(참조·Soft·ClassRef)로 판정한다. 생성 함수표는 codec·원소 native 키와 경로 thunk(대상 종류 = 원소 종류 — `[i]` 읽기·쓰기·멤버만, 삽입·삭제·비우기는 Unsupported)이고 생성 Trace는 `TraceArrayField`(C 배열 오버로드)를 부른다.

리플렉션 클래스·struct는 정의 안에 `ROCKS_GENERATED_BODY()`를 둔다(빌드의 메타 단계가 없으면 RMC2211로 거부한다 — MetaCompiler `--require-generated-body`. Binds 미러 선언은 예외다). 이 매크로는 private 필드 접근 권한과 함께, 타입이 리플렉션 타입임을 선언만으로 알 수 있는 표지를 남긴다. 그래서 리플렉션 헤더(`Foo.h`)만 include하고 생성 헤더(`Foo.generated.h`)를 빼먹은 번역 단위에서 그 타입으로 값 복사·필드 형질·렌더 전달 검사 같은 리플렉션 템플릿을 쓰면 컴파일 오류가 나고, 오류가 include할 헤더를 안내한다([생성 코드 설계](../Design/ReflectionGeneratedSources.ko.md) 8.1절).

단일 public 비가상 상속만 지원한다(리플렉션 struct의 상속은 리플렉션 B9k가 열었다 — 아래 struct 항목, ReflectionExtensions 12절). 기반 타입과 `ObjectRef<T>`·`SoftObjectRef<T>`의 대상은 `Object`, 같은 모듈의 리플렉션 타입, 또는 import한 의존 모듈 descriptor의 public 리플렉션 타입이어야 한다(아래 "모듈 메타데이터"). 파생 스키마는 상속 필드를 안정 ID 순서로 펼치며, 상속을 포함한 필드 수는 최대 4,096개다. 생성 접근기는 멤버 포인터를 사용한다. 객체 레이아웃에서 offset을 추정하거나 raw 필드 주소를 반환하지 않는다.

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
| `Name="…"` | CLASS·STRUCT | 타입 도구 이름(`TypeSchema::toolName`, 리플렉션 B9d2 — 에셋 명령의 `<Type>`·ClassRef 텍스트·InstancedStruct `_structType`·Sparks가 쓴다). `.`으로 이은 ASCII 식별자 마디(`Weapons.Sword`)이고 `none`은 대소문자 무시로 예약이다. 없으면 한정하지 않은 C++ 타입 이름이다. 클래스와 struct가 한 이름 공간이며 이 모듈의 타입끼리, import한 타입과의 사이, import한 모듈끼리 대소문자 무시로 유일해야 한다(RMC2203 — 런타임 `TypeRegistry` 등록은 AlreadyExists, 조회는 `AcquireByToolName`). 생성 스키마를 복사해 같은 C++ 타입을 다른 TypeId로 등록하는 별칭 스키마는 `toolName`을 비운다. 직렬화는 TypeId를 쓰므로 이름과 무관하다. |
| `WorldOnly` | CLASS | `TypeFlags::WorldOnly`. **상속된다** — base가 WorldOnly면 파생도 자동으로 WorldOnly다(TypeRegistry도 수동 등록에서 확인). 에셋 객체 Registry의 생성 거부는 `RegistryPolicy::rejectWorldOnly`(Unsupported — [ObjectFoundations](../Design/ObjectFoundations.ko.md) 1.3절)다. |
| `Transient`·`Deprecated` | CLASS | `TypeFlags::Transient`(4)·`Deprecated`(8), 리플렉션 B9h2. **상속된다**. 저장 불가 클래스다 — 그래프 저장 정책 `UnsaveablePolicy::Exclude`(기본)가 그 객체를 집합에서 거부하고 그것을 가리키는 참조를 null + 진단으로 쓴다(Keep은 보통 노드). 로드는 진단(TransientType·DeprecatedType)과 함께 된다. Deprecated 클래스는 ClassRef 쓰기가 거부하고, Deprecated가 아닌 타입의 PROPERTY가 Deprecated 클래스를 가리키면 RMC2209다. |
| `Within="…"` | CLASS | `TypeSchema::withinType`(리플렉션 B9h2). Outer가 그 클래스 또는 파생이어야 한다(Outer 없음 포함 InvalidArgument — 생성 정체성 규칙). 상속되고 파생이 다시 쓰면 base Within의 파생이어야 한다(RMC2208). 철자는 Clang이 정규 이름으로 푼다. |
| `Atomic` | STRUCT | `TypeFlags::Atomic`(16, 리플렉션 B9b2). 값이 **통째 단위**다 — 차이 저장은 멤버 하나라도 기준과 다르면 모든 멤버를 쓰고 override 집합·되돌리기의 단위가 그 struct 필드 경로다(구현은 A5 — 되돌리기는 [런타임 클래스 메타데이터](../Design/RuntimeClassMetadata.ko.md) "struct 통째 되돌리기" 후속과 함께, [리플렉션 B9](../Design/ReflectionExtensions.ko.md) 3.1절). 로드·경로 접근은 멤버 단위 그대로다. 조회는 `StructView::IsAtomic()`이고 멤버에서 바깥으로 전파하지 않는다. CLASS에 쓰면 모르는 지정자(RMC2000)다. |
| `Settings(Branch="…", Section="…")` | STRUCT | 설정 struct다([설정 계층](../Design/ConfigSystem.ko.md) 7절 — branch 파일 `<Branch>.json`의 최상위 키 `Section`이 이 struct 값이다). 생성 `GeneratedType<S>::Settings()`가 `SettingsSchema`(`Meta/SettingsSchema.h` — branch·섹션 이름, 설정 지정자가 있는 직접 멤버의 필드 ID·`SettingsFieldFlags`·콘솔 이름)를 낸다. 괄호 안 지정자 목록(묶음 값)을 받는 유일한 지정자다(묶음 안의 묶음·다른 지정자의 묶음 값은 RMC2000). Branch·Section은 필수 ASCII 식별자이고 Section `Settings`(대소문자 무시)는 콘솔 설정 이름 → 값 섹션으로 예약이다(RMC2210). 설정 struct는 값이므로 강한 참조·Registry 컨테이너를 담을 수 없다(RMC2210). descriptor에는 싣지 않는다(import 모듈은 그 모듈의 생성 헤더를 쓴다). |
| `Transient` | PROPERTY | GC에서 추적하지만 저장 그래프에서 뺀다. |
| `ReadOnly` | PROPERTY | 일반 write는 거부하고 생성·복원의 staged write만 허용한다. |
| `EditorOnly` | PROPERTY | 플래그만 단다(항상 컴파일). 쿠킹 데이터에서 빼는 것은 그래프 저장 정책 `FieldEncodePolicy::excludedFlags`(쿠킹 = EditorOnly — 그 필드를 저장하지 않는다)이고, 쿠킹 로드는 `FieldDecodePolicy::excludedFlags`로 그런 필드가 있으면 InvalidArgument다([ObjectFoundations](../Design/ObjectFoundations.ko.md) 4절 — 정책을 주지 않으면 보통 필드처럼 저장·복원한다). |
| `Required` | PROPERTY | `ObjectRef<T>`·`SoftObjectRef<T>`·`ClassRef<Base>` 필드와 그 원소의 배열(`ObjectRefArray<T>`·`Array<SoftObjectRef<T>>`·`Array<ClassRef<Base>>` — 원소마다 non-null), 그 원소의 Set(`OrderedSet<SoftObjectRef<T>>`·`OrderedSet<ClassRef<Base>>` — 원소마다)과 그 값의 Map(`OrderedMap<K, SoftObjectRef<T>>`·`OrderedMap<K, ClassRef<Base>>`·`ObjectRefMap<K, T>` — 값마다, 키에는 Required가 없다 — 리플렉션 B9f2·B9f4)에만 쓴다(그 밖은 RMC2202 — `WeakObjectRef<T>`도 대상이 죽으면 저절로 null이 되므로 거부한다). 패키지 저장·쿠킹이 null을 거부한다(A1). 로드는 거부하지 않는다. |
| `DuplicateTransient`·`NonTryoutDuplicateTransient` | PROPERTY | `FieldFlags` 16·32(리플렉션 B9h1). 복제 저장에서 뺀다 — 복제 호출자가 `excludedFlags`로 고른다(에셋 복제 = 둘 다, Tryout 복제 = DuplicateTransient). `Required`와 함께 쓰면 RMC2202(복제본이 늘 null이다 — TypeRegistry 등록도 InvalidArgument). |
| `SaveGame` | PROPERTY | `FieldFlags` 64(리플렉션 B9h1). 세이브게임 저장의 포함 필터 `includeFlags`(`FieldEncodePolicy`·`FieldDecodePolicy`)가 필드·struct 멤버마다 자기 플래그로 본다(UE처럼 멤버마다 표시 — Atomic struct 안의 멤버는 통째라 보지 않는다). |
| `TextExportTransient` | PROPERTY | `FieldFlags` 128(리플렉션 B9h1). 복사 텍스트에서 뺀다(텍스트 codec B9i가 쓴다). |
| `Name="…"` | PROPERTY | 도구·명령행 이름(`FieldSchema::toolName`). ASCII 식별자이며, 없으면 C++ 필드 이름이다. 상속 필드를 포함해 대소문자 무시로 유일해야 한다(RMC2203). 직렬화는 필드 ID를 쓰므로 이름과 무관하다. |
| `Category="…"`·`DisplayName="…"` | PROPERTY | 표시용 UTF-8 문자열(아래). |
| `MaxElements=N` | PROPERTY | 배열·Map·Set 필드의 원소 수 상한(그래프 저장과 B6 경로 쓰기에서 건다 — 로드와 Registry 변경 API는 걸지 않는다, Map·Set은 리플렉션 B9f2). 1..1,048,576(넘으면 RMC2000), 없으면 65,536. 배열·Map·Set이 아닌 필드는 RMC2202(종류를 먼저 판정한다). |
| `UserConfig`·`ConsoleName="…"`·`RestartRequired`·`StartupOnly`·`RenderRead`·`DevelopmentOnly` | PROPERTY | 설정 지정자([설정 계층](../Design/ConfigSystem.ko.md) 7절). 설정 struct의 직접 멤버에만 쓴다(그 밖은 RMC2210). `FieldFlags`가 아니라 `SettingsFieldSchema::flags`(`SettingsFieldFlags` — UserConfig 1·RestartRequired 2·StartupOnly 4·RenderRead 8·DevelopmentOnly 16)와 `consoleName`에 실린다. `ConsoleName`은 소문자 점 경로 `[a-z][a-z0-9_]*('.' …)+`(마디 둘 이상)이고 struct 안에서 유일하며(RMC2210 — 영역 이름 공간 표와 실행 전체 유일성은 설정 계층이 등록 때 본다), `ConsoleName`·`RenderRead`는 bool·int32·int64·float·double·String·Name·리플렉션 enum 필드에만 쓴다(RMC2202). `RestartRequired`와 `StartupOnly`는 함께 쓸 수 없다(RMC2210). |

`ROCKS_NATIVE()`는 저장·리플렉션하지 않는 런타임 멤버(GPU 핸들·캐시)이며 지정자를 받지 않는다. GC가 추적하지 않으므로 강한 객체 참조를 담을 수 없다: MetaCompiler가 Clang AST로 멤버 타입을 값으로 담는 필드·base·이름 없는 struct/union 멤버·배열 원소·템플릿 인자와 패턴 필드까지 재귀 검사해 `ObjectRef`·`RootRef`·`ObjectRefArray`·`StructRefArray`나 Object 파생 타입의 raw 포인터·참조가 있으면 RMC2205로 거부한다(`Array<참조 struct>`는 Clang의 `Array` static_assert가 먼저 실패하며 MetaCompiler가 그 진단을 RMC2205로 바꾼다 — 아래 "리플렉션 배열"). 포인터·참조는 대상이 Object 파생이거나 대상이 다시 강한 참조(`T**`·`ObjectRef<T>*`)인지만 본다 — 빌린 포인터가 가리키는 비-Object 레코드(예: `World*`)의 필드는 그 소유자가 추적한다. 직접 멤버 타입이 (여러 겹의 포인터·참조와 cv·`__restrict`·`__unaligned`·주소 공간·`_Atomic`·배열을 거쳐) Object 파생 class나 강한 참조 래퍼를 가리키는지는 MetaCompiler의 탐침(`NativeMemberProbe`)이 판정한다: 첫 실행과 같은 번역 단위 끝에 멤버마다 `decltype(::Owner::member)` 판정 변수를 붙여 Clang을 한 번 더 실행하므로(경고 끔, 그 변수만 덤프, 탐침 이름은 모두 `rocks_meta_probe`·`RocksProbe`·`rocksProbe` 접두라 헤더의 매크로와 겹치지 않는다) 같은 namespace의 이름·inline namespace·별칭·부분/명시 특수화(식·enum·char 인자, 기본 인자)·CRTP·pack base를 Clang이 푼다. private 멤버는 `ROCKS_GENERATED_BODY`의 friend인 `GeneratedType` 특수화 안에서 본다. 대상 템플릿의 인스턴스화가 실패하면(hard error·재귀 깊이) 진단의 instantiation note를 탐침 변수 줄에 대응시켜 그 멤버만 RMC2205 "cannot verify (Clang: 그 멤버의 error 줄)"이고 나머지 멤버는 판정대로다(대응하지 않으면 묶음을 반으로 나눠 다시 판정한다. 판정 템플릿 자체가 컴파일되지 않으면 RMC3000 한 번). 대상 class가 불완전하면 모듈의 public 헤더를 더한 두 번째 탐침으로 다시 본다 — public 입력 헤더 철자의 첫 디렉터리 아래 `.h`(private 헤더 제외)에서 입력·생성 헤더와 이 모듈의 생성 헤더 철자를 직접 include하는 헤더를 빼고, 탐침 include 검색 순서에서 이 모듈의 출력 루트를 빼(빌드 상태와 무관하게 같은 판정), 실패하면 같은 격리와 함께 컴파일되지 않는 헤더를 하나씩 걸러 다시 실행한다. 그래도 불완전하거나 그 멤버의 탐침이 실패하면 허용한다(`World*` 같은 전방 선언 back-pointer, private 헤더·.cpp에만 정의가 있는 대상 포함). 대상 템플릿의 특수화가 입력(과 그 public 헤더)이 include하지 않는 헤더에만 있으면 탐침은 primary 템플릿으로 판정한다. Object 파생 타입은 값이나 템플릿 인자로 닿아도 펼치지 않고, `WeakObjectRef`·`SoftObjectRef`·`ObjectHandle`은 허용한다(대상을 담지 않으므로 템플릿 인자도 따라가지 않는다). `static constexpr bool RocksRequiresWorldOnlyOwner = true;`를 선언한 타입(을 값으로 담는 타입)은 WorldOnly 클래스의 NATIVE 멤버로만 둘 수 있다(RMC2206). 생성 코드는 직접 멤버 타입의 강한 참조 래퍼(배열 원소, 래퍼를 가리키는 포인터와 참조 포함, 담는 타입의 필드는 제외)와 WorldOnly 형질을 `static_assert`(`IsNativeStrongReference`·`RequiresWorldOnlyOwner`, `<Object/NativeMemberTraits.h>`)로 한 번 더 둔다 — 형질은 대상 타입의 완전성에 기대지 않으므로(TU마다 값이 갈리면 ODR 위반이다) Object 파생 raw 포인터는 탐침만 판정한다. NATIVE 멤버는 영속 필드가 아니고 강한 간선을 담지 않으므로 생성 타입의 `completeFieldLayout`은 계속 true다. 수동 등록 매크로는 `ROCKS_NATIVE_TYPE`이다(이전 `ROCKS_NATIVE_CLASS`).

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
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY() TextureGroup group = TextureGroup::World;
};
```

`ROCKS_ENUM(...)`은 바로 뒤의 enum 정의 하나에 붙는다(`ROCKS_CLASS`와 같이 사이에는 공백·주석만, 같은 소스·스코프). 클래스 안의 중첩 enum도 되지만 public 구역이어야 한다(아니면 RMC2401 — 생성 코드가 바깥에서 이름을 쓴다). 대상은 underlying을 enum 이름 바로 뒤에 원문으로 명시한 `enum class`(또는 `enum struct`)이고 underlying은 UE와 같은 정수 8종 `uint8`·`uint16`·`uint32`·`uint64`·`int8`·`int16`·`int32`·`int64`를 받는다(리플렉션 B9a2 — 생략과 그 밖의 형식, 예를 들어 평범한 `char`·`bool`·`long`(플랫폼마다 폭이 다르다)은 RMC2401). 저장 값이 int64이므로 `uint64` enum의 열거자 값은 int64 최댓값 이하여야 하고(넘으면 RMC2401) Flags `uint64` enum은 비트 0–62를 쓴다. 열거자는 하나 이상, 4,096개 이하이며 값이 서로 달라야 한다(별칭은 RMC2401). 초기값이 없는 열거자는 이전 값 + 1이다. 지정자는 `ToolPrefix="…"`(`[A-Za-z0-9_]+`)와 `Flags`(비트 플래그 — 열거자 값은 음수일 수 없고, 유효한 값은 선언 값들의 OR이며 0은 항상 유효)다. 열거자에는 이름 바로 뒤에 `ROCKS_ENUMERATOR(Name="…", DisplayName="…", Hidden)`를 붙일 수 있다. `Name`은 도구 이름(`[A-Za-z0-9_]+`, 숫자로 시작해도 된다 — 접두를 뗀 UE 이름 `8BitData`)이고 없으면 C++ 이름이다. `DisplayName`은 표시용 UTF-8, `Hidden`은 저장 값으로는 유효하지만 도구 설정이 받지 않는 열거자다(판정은 설정 계층). 접두가 붙은 이름과 뗀 이름을 합친 도구 이름 집합은 대소문자 무시로 유일해야 한다(RMC2203). enum도 타입 ID를 identity 명세의 `type` 항목으로 가진다(`ids add --type <정규 이름>`). 열거자는 ID가 없다 — 저장은 값으로 하므로 열거자 이름을 바꿔도 데이터가 유지되고, 값을 바꾸면 저장된 데이터의 뜻이 바뀐다.

생성 헤더는 `GeneratedType<E>`에 `Id()`, `Schema()`(`EnumSchema` — 이름·underlying·플래그·접두와 열거자의 이름·도구 이름·표시 이름·값·플래그), constexpr `IsDeclared(int64)`, `Register(TypeRegistry&, ModuleLease = {})`를 낸다. C++ underlying이나 열거자 값이 바뀌었는데 생성물을 다시 만들지 않으면 생성 헤더의 `static_assert`가 실패한다(MetaCompiler가 Clang AST에서 읽은 값을 컴파일러가 한 번 더 대조한다). `IsDeclaredEnumValue(schema, value)`와 `FindEnumerator(schema, toolName)`(`Meta/TypeSchema.h`)은 같은 규칙의 실행 시 버전이다 — 도구 이름 조회는 ASCII 대소문자를 무시하고 접두가 있든 없든 받으며, 숨김 열거자도 찾는다.

enum은 그 enum을 필드로 쓰는 클래스보다 먼저 등록한다(`TypeRegistry::RegisterEnum`, 모듈 집계 함수는 enum을 먼저 등록한다). 클래스 등록은 Enum 필드의 `value.type`이 등록된 enum 레코드여야 하고(없으면 NotFound, 클래스 레코드면 TypeMismatch), 생성 접근기가 싣는 C++ enum의 native 키(`RuntimeFieldOps::valueNativeType`)가 그 레코드의 키와 같아야 한다(다르면 TypeMismatch). 등록된 클래스는 쓰는 enum을 의존으로 붙잡으므로 그 enum의 `Unregister`는 Busy다(lease가 아니므로 `Shutdown`을 막지 않는다). `Acquire`는 클래스만 돌려주고(enum ID면 TypeMismatch), enum은 `AcquireValueType(id, TypeKind::Enum)`으로 열어 `TypeLease::Enum()`으로 스키마를 읽는다. 클래스 lease의 `FieldEnum(fieldIndex)`는 그 필드가 가리키는 enum의 스키마다.

Enum 필드의 `FieldValue`는 `kind = ValueKind::Enum`, `signedInteger = 값`, `type = enum ID`다. 쓰기(등록된 `write`·`writeStaged`·등록 전 callback)는 `type`이 필드의 enum ID와 다르면 TypeMismatch, 선언되지 않은 값(범위 밖 포함)이면 InvalidArgument이며 값은 그대로다. 그래프 저장은 C++에서 `static_cast`로 넣은 선언되지 않은 값을 InvalidArgument로 거부하고, 복원은 기본 정책에서 저장된 선언되지 않은 값을 객체를 만들기 전에 InvalidArgument로 거부하고, `UndeclaredEnumPolicy::ReportAndDefault`면 `UndeclaredEnumValue`를 진단한 뒤 대체 값을 쓴다([Serialization](Serialization.ko.md) "필드 codec과 문맥").

### 리플렉션 struct(`ROCKS_STRUCT`)

```cpp
ROCKS_STRUCT(Version = 2)
struct BulkDataRef {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() rocks::uint32 bulkId = 0;
    ROCKS_PROPERTY() rocks::Sha256Digest digest;
};

ROCKS_CLASS()
class TextureSource : public rocks::Object {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY() BulkDataRef bulk;
};
```

`ROCKS_STRUCT(...)`은 `ROCKS_CLASS`와 같은 규칙으로 바로 뒤의 완전한 `class`/`struct` 정의 하나에 붙는다(한 레코드에 둘을 함께 쓰면 거부한다). 대상은 비템플릿 타입이고(템플릿은 RMC2000), base는 리플렉션 struct 하나(public 비가상 — 그 필드를 같은 ID로 펼쳐 담고 Atomic을 상속한다)와 필드·virtual 함수 없는 비리플렉션 base만 둘 수 있다(그 밖은 RMC2301 — 리플렉션 클래스·Binds 미러 base, Binds 미러의 base, virtual 함수 포함. 비리플렉션 base가 비었는지는 생성 `static_assert(IsEmptyV && !IsPolymorphicV)`가 본다 — 리플렉션 B9k, Docs/Design/ReflectionExtensions.ko.md 12절), 모든 비정적 데이터 멤버는 `ROCKS_PROPERTY` 또는 `ROCKS_NATIVE`이며 `explicit S(Allocator&) noexcept` 또는 `S() noexcept` 생성자와 noexcept 소멸자가 있어야 한다(생성 `MakeRuntimeStructOps<S>`의 `static_assert`). `ROCKS_GENERATED_BODY()`는 클래스와 같이 모든 리플렉션 struct가 둔다(RMC2211 — Binds 미러만 예외). private 멤버 접근도 이 매크로가 연다. 클래스 안에 중첩한 리플렉션 struct(와 클래스)는 public 구역이어야 한다(RMC2000 — 생성 코드가 바깥에서 이름을 쓴다). 지정자는 `Version=N`(없으면 1), `Atomic`(값 없음 — 위 지정자 표, 생성 스키마 flags 16·descriptor flags `Atomic`)과 `Binds="…"`(Core 값 타입 결합 — 아래 "Core 값 타입")이다. TypeRegistry `RegisterStruct`는 flags에 Atomic만 받는다(클래스 플래그·그 밖의 비트는 InvalidArgument). 클래스 `Register`는 Atomic 비트를 거부한다. struct는 클래스의 base나 `ObjectRef<T>`·`SoftObjectRef<T>`의 대상이 될 수 없다(RMC2003). struct 멤버로 다른 struct를 담을 수 있고 중첩 깊이(1 + 멤버 struct 깊이의 최댓값)는 `MaxStructDepth` = 16 이하다(RMC2303). 순환은 RMC2302다 — 일반 값 멤버로는 생기지 않지만 Binds 미러가 자기가 묶는 실제 타입을 담으면 생기고, import한 descriptor에서도 검사한다. struct 타입과 필드도 identity 명세의 `type`·`field` 항목이다.

생성 형질: `ObjectRef` 멤버(Transient 포함)·`ObjectRefArray`·`ObjectRefMap` 멤버나 그런 struct 멤버·원소 struct를 담으면 `ContainsStrongReference`(참조 struct — 소유 클래스의 생성 Trace가 struct의 Trace를 거쳐 방문한다), `RocksRequiresWorldOnlyOwner` 형질 NATIVE 멤버나 그런 struct 멤버·원소 struct를 담으면 `RequiresWorldOnlyOwner`, Registry 컨테이너(`ObjectRefArray`·`StructRefArray`·`ObjectRefMap`) 멤버나 그런 struct 멤버를 담으면 `ContainsReferenceArray`다(아래 "리플렉션 배열" — 그런 struct는 StructRefArray 원소가 될 수 없다, RMC2304). struct의 WorldOnly 소유자 형질은 오류가 아니라 전파되며 그 struct를 필드(상속 필드·NATIVE 멤버 포함)로 둔 WorldOnly 아닌 클래스가 RMC2206이다. NATIVE 멤버로 둔 참조 struct는 RMC2205다. 다른 모듈의 private struct, 같은 모듈 public 타입의 private struct 필드·NATIVE 멤버는 RMC2006이다.

생성 헤더는 `GeneratedType<S>`에 `ContainsStrongReference`·`RequiresWorldOnlyOwner`·`ContainsReferenceArray`·`Depth`(constexpr), `Id()`, `Schema()`(base 없는 `TypeSchema` — flags는 `Atomic`이면 16, 아니면 0, 끝 칸이 실효 타입 도구 이름), `Trace(const S&, TraceVisitor&)`(모든 struct — 참조 없는 struct는 빈 본문), `Ops()`(`RuntimeStructOps` — 멤버마다 `detail::StructFieldOps<&S::m>`, 참조 struct만 `trace`), `Register`를 낸다. 형질과 깊이는 MetaCompiler 모델 값이고 `static_assert`로 대조한다(import descriptor의 flags를 믿지 않는다): 직접 멤버 struct와 원소 struct의 생성 값을 포함하는지, 그리고 PROPERTY 멤버마다 C++ 멤버 타입(`IsNativeStrongReference`·`detail::RegistryContainer`)과 맞는지 본다(Binds 미러는 `VerifyBinds`의 멤버 형식 대조가 같은 보장을 하므로 멤버 타입 대조를 내지 않는다 — 아래 "리플렉션 배열"). struct가 `Migrate`라는 이름의 멤버(메서드·함수 템플릿, 접근 무관)를 선언하면 MetaCompiler가 AST에서 그것을 보고 생성 `Ops()`가 `ops.migrate = &S::Migrate;`(타입 마이그레이션)를 낸다. 시그니처는 `static Result<Array<uint8>> Migrate(Allocator&, TypeId, uint32 from, uint32 to, Span<const uint8> record) noexcept`여야 하며 다르면(noexcept 없음·비정적 포함) 생성 코드가 컴파일되지 않는다. private·protected `Migrate`는 struct의 `ROCKS_GENERATED_BODY()`(모든 리플렉션 struct가 둔다 — RMC2211)로 생성 특수화가 friend가 되어 주소를 얻는다. 클래스의 struct 필드는 `read`·`write`·`writeStaged` 없이 codec과 struct native 키(`valueNativeType`)만 싣는다(쓰기는 Unsupported — 멤버 단위 쓰기는 경로 접근 B6). 다른 헤더(같은 모듈 포함)나 import base의 상속 필드가 쓰는 struct는 그 생성 헤더를 include하고, 한 헤더 안의 특수화는 쓰는 struct(PROPERTY 멤버와 NATIVE 리플렉션 struct 멤버 — 생성 `static_assert`가 둘 다 쓴다)가 먼저 나온다. 모듈 집계 함수는 enum → struct(`ModuleRegistration::RegisterStructIfAbsent`, 같은 순위 — 1 + 이 모듈의 쓰는 struct 순위 최댓값 — 순·같은 순위는 이름 순) → 클래스 순으로 등록한다. 저장·복원·버전 정책은 [Serialization](Serialization.ko.md)의 "리플렉션 struct 값"이다.

### Core 값 타입(`CoreTypeBindings`, `Binds`)

Core 수학 타입은 MetaSchema·ObjectRuntime에 의존하지 않는 Core에 있어 annotation을 달 수 없으므로, Metadata 모듈 `CoreTypeBindings`(Core/Source/Runtime/CoreTypeBindings, 헤더만, Engine SDK 타깃에 포함)의 `CoreTypeBindings/CoreValueTypes.h`가 미러 선언으로 리플렉션한다. 대상: `Vector2·Vector3·Vector4<float|double>`(별칭 `Vec2`·`Vec2d`·`Vec3`·`Vec3d`·`Vec4`·`Vec4d`), `Quaternion<float|double>`(`Quat`·`Quatd`), `Rotator<float|double>`(pitch·yaw·roll 도 — UE FRotator와 같음), `Color`(uint8 RGBA), `LinearColor`(float RGBA), `IntVec2`·`IntVec3`(int32), `Transform`(`Quatd rotation`·`Vec3d translation`·`Vec3d scale` — UE FTransform과 같은 순서·double, 다른 미러에 의존), `Transform3f`(`Transform3<float>` — `Vec3`·`Quat`·`Vec3`)·`Transform3d`(`Transform3<double>` — `Vec3d`·`Quatd`·`Vec3d`)의 17종이고 멤버는 Core 선언 순서다. `Transform`을 뺀 16종은 `ROCKS_STRUCT(Binds = "…", Atomic)`이다(UE가 수학 타입을 native Serializer·Immutable 이진으로 통째 쓰는 결과를 따른다). `Transform`은 UE `FTransform`처럼 비Atomic(멤버 단위)이고 그 멤버 미러가 각각 Atomic이다(리플렉션 B9b2 — [리플렉션 B9](../Design/ReflectionExtensions.ko.md) 3.2절). Core 값 타입 필드를 리플렉션하는 모듈은 `CoreTypeBindings`에 의존한다(의존하지 않고 그런 필드를 쓰면 "unsupported field type" 진단이 의존을 더하라고 안내하고, 의존하는데 묶이지 않은 인스턴스 — 예: `Vector3<int>` — 를 쓰면 묶인 인스턴스를 알려 준다). 필드에는 실제 타입(별칭 포함)을 쓰고 미러 타입(`…Binding`)을 직접 쓰면 거부한다(RMC2000).

```cpp
ROCKS_STRUCT(Binds = "rocks::Vec3", Atomic, Version = 1)
struct Vec3Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
};
```

- `Binds`는 `CoreTypeBindings` 모듈에서만 받는다(다른 모듈은 RMC2004 — 무관한 두 모듈이 같은 타입을 묶으면 메타데이터가 두 번 정의된다). 한 실제 타입은 미러 하나만 묶고(별칭으로 같은 타입을 두 번 묶어도 RMC2004), 리플렉션 타입을 묶을 수 없다(RMC2004). 철자는 전역 기준 완전 한정 이름(앞의 `::`는 있어도 없어도 된다 — 없으면 붙여 해석한다)과 템플릿 인자만 받는다(그 밖은 RMC2000).
- 철자는 문자열로 비교하지 않는다. MetaCompiler가 같은 합성 번역 단위 끝(전역 범위)에 미러마다 `using rocksBinds_N = ::<철자>;`와 완전한 class 타입인지 확인하는 `static_assert`를 붙여 Clang을 한 번 더 돌리고(`-ast-dump-filter`) 별칭의 desugar 철자를 정규 철자로 쓴다(`rocks::Vec3` → `rocks::Vector3<float>`). 이 실행의 오류는 그 미러 선언 위치의 RMC3000이다. 정규 철자가 descriptor `binds` 문법(한정 이름과 이름·음이 아닌 정수 템플릿 인자) 밖이면(예: 별칭으로 묶은 `Fixed<-3>`) 그 미러 위치의 RMC2000이다 — 의존 모듈이 descriptor를 읽지 못하게 되지 않도록 생성 모듈에서 막는다. 필드 형식은 이 정규 철자로 미러를 찾는다(이 모듈과 import 모두). 정규 철자는 별칭의 desugar가 아니라 정규 형식 래퍼(`rocksBinds_canon::RocksCanonType<N, rocksBinds_N>`)의 템플릿 인자로 읽는다(Clang 22는 비템플릿 타입을 가리키는 별칭의 desugar 철자를 한정하지 않는다 — [Clang 버전 지원](../Design/ClangVersionSupport.ko.md)).
- 생성 코드는 미러가 아니라 실제 타입의 `GeneratedType`을 내고 멤버 thunk·Trace·생성·소멸·native 키가 모두 실제 타입을 쓴다(`ClassModel.NativeName`). 스키마 이름은 미러 이름이다. 생성 헤더의 비템플릿 함수 `VerifyBinds`가 실제 타입에 대해 멤버마다 `IsSameV<decltype(Real::m), T>`(이름·형식)와 `IsSameV<decltype(&Real::m), T (Real::*)>`(base가 아니라 실제 타입에 직접 선언), 구조적 바인딩(`[[maybe_unused]] const auto& [m0, …] = value;`)과 위치별 형식(개수 — private 데이터 멤버·union·base 분할도 실패), `__is_standard_layout`과 연속 멤버의 `ROCKS_OFFSET_OF` 증가 사슬(선언 순서 — 같은 형식 멤버끼리 바뀐 순서 포함)을 확인한다 — 이름·형식·개수·순서가 다르면 컴파일되지 않는다. sizeof는 대조하지 않는다(꼬리 패딩의 숨은 멤버는 구조적 바인딩 개수가 잡는다). 생성·소멸 noexcept는 실제 타입에 건다. 미러에는 NATIVE 멤버와 `Migrate`를 두지 않는다(RMC2000).
- descriptor struct 항목의 `binds`가 정규 철자다(미러가 아니면 "", 앞에 `::`가 없다). import는 철자 문법을 검사하고(RMC1000) 모듈·중복·리플렉션 타입 결합을 RMC2004로 거부한다. 저장은 다른 struct와 같은 태그형 중첩 Record다(고정 배치를 쓰지 않는다).

`Category`와 `DisplayName`은 바이트 길이를 가진 UTF-8 데이터이며 중간 NUL(`\u0000`)도 유효하다. `Span<const char>`의 길이를 사용해야 하며 NUL 종료 문자열 함수로 읽으면 값이 잘린다. 등록 시 이 바이트를 깊게 복사한다. 생성 C++은 한글 등 읽을 수 있는 문자를 유지하고 인용부호·역슬래시·제어 문자를 escape한다. 제어 바이트는 정확히 세 자리 8진수로 표현하므로 뒤에 숫자가 와도 서로 합쳐지지 않는다.

### 리플렉션 배열(`Array`·`ObjectRefArray`·`StructRefArray`)

```cpp
ROCKS_STRUCT()
struct MaterialSlot {
    ROCKS_GENERATED_BODY()

    explicit MaterialSlot(rocks::Allocator& allocator) noexcept : name(allocator) {
    }

    ROCKS_PROPERTY() rocks::ObjectRef<Material> material;
    ROCKS_PROPERTY() rocks::String name;
};

ROCKS_CLASS()
class Mesh : public rocks::Object {
    ROCKS_GENERATED_BODY()

public:
    explicit Mesh(rocks::Allocator& allocator) noexcept
        : lods(allocator), overrides(allocator), slots(allocator) {
    }

    ROCKS_PROPERTY(MaxElements = 8) rocks::Array<float> lods;
    ROCKS_PROPERTY() rocks::ObjectRefArray<Material> overrides; // index = 슬롯, null = 재정의 없음
    ROCKS_PROPERTY() rocks::StructRefArray<MaterialSlot> slots;
};
```

배열 필드는 세 컨테이너 중 하나다([리플렉션·빌드 보강](../Design/ReflectionFoundations.ko.md) 5절). `Array<T>`의 원소는 스칼라·`Guid`·`String`·`Name`·`AssetId`·`Sha256Digest`·`ROCKS_ENUM` enum·`SoftObjectRef<U>`·`WeakObjectRef<U>`·`ClassRef<B>`·참조 없는 `ROCKS_STRUCT`(Core 값 타입 포함 — `Array<rocks::Vec3>`)이고, 객체 참조 목록은 `ObjectRefArray<T>`(원소 null 허용, `ObjectRefArray<>`는 대상 `Object`), 참조 struct 목록은 `StructRefArray<S>`(Registry가 관리하는 원소 블록 — 원소 주소 고정)다. 컨테이너는 원소 종류와 원소 struct 형질로 정해진다: 원소가 강한 객체 참조면 `ObjectRefArray`(`WeakObjectRef` 원소는 GC 간선이 아니므로 `Array`), 원소 struct가 `ContainsStrongReference`면 `StructRefArray`, 그 밖은 `Array`다. 다른 컨테이너를 쓰면 RMC2304다(`Array<ObjectRef<T>>`, `Array<참조 struct>` — noexcept 이동을 직접 정의해 Clang을 지나도 막는다 —, `StructRefArray<참조 없는 struct>`). 원소 철자는 Clang의 desugar 정규 철자로 해석하므로 세 컨테이너와 `ObjectRef`·`WeakObjectRef`·`SoftObjectRef`·`ClassRef`의 명시적 인스턴스화·`extern template`은 Policy 검사(`reflection-container-instantiation` — 주석·문자열을 지운 텍스트에서 여러 줄 선언까지 찾는다)가 저장소 전체에서 거부한다(있으면 Clang이 작성 철자를 출력해 원소 해석이 어긋날 수 있다 — 원소를 찾지 못하면 RMC2000 안내에 이 원인을 적는다).

- 원소 제한: 중첩 배열과 컨테이너 인자의 배열은 RMC2304(`ObjectRefArray<Array<…>>`도 RMC2003보다 먼저), 맨 위 cv 원소(`Array<const T>`·`ObjectRefArray<const T>`·`StructRefArray<const S>`)는 RMC2000(포인터 대상의 const는 지원하지 않는 원소), 지원하지 않는 원소는 원소 철자를 담은 RMC2000, `ObjectRefArray<T>`의 T가 클래스가 아니면 RMC2003, `StructRefArray<S>`의 S가 리플렉션 struct가 아니거나(enum·클래스·스칼라 포함) Registry 컨테이너를 (중첩까지) 담으면 RMC2304다. Registry 컨테이너를 담는 참조 struct 원소는 어느 컨테이너로도 쓸 수 없으므로 철자 컨테이너 불일치보다 먼저 그 사유로 진단한다.
- `Array<ObjectRef<T>>`·`Array<참조 struct>`·`Array<ObjectRefArray를 담은 struct>`는 멤버 선언에서 Core `Array`의 nothrow-move static_assert로 Clang이 먼저 실패한다(오류의 주 위치는 `Array.h`이고 멤버는 note에만 나온다). MetaCompiler는 첫 Clang 실행의 첫 error가 Core `Containers/Array.h`에 있고 그 error에 딸린 note(다른 진단이 끼기 전) 중 이 모듈 입력 헤더 안의 위치가 있으면(clang `file:line:col`·clang-cl `file(line,col)` 모두, 지역화될 수 있는 문구는 보지 않는다) 실패한 실행의 AST에서 그 위치에 이름이 있는 필드를 찾고, 같은 AST로 NATIVE 재귀 검사와 같은 규칙을 원소(NATIVE는 필드 형식 전체)에 적용해 강한 참조 경로가 있을 때만 멤버 선언 위치의 RMC2304(필드 이름·원소 철자·경로)로, NATIVE 멤버면 RMC2205로, PROPERTY·NATIVE가 함께 붙었으면 RMC2204로 바꾼다. 경로가 없거나(참조 없는 원소의 이동·소멸 noexcept 위반) 필드를 찾지 못하거나 보조 조회가 실패하면 Clang 원문을 담은 RMC3000을 유지한다. 안내("참조는 ObjectRefArray, 참조 struct는 StructRefArray")는 RMC2304 힌트에 있고 Core `Array.h`의 메시지는 ObjectRuntime 타입 이름을 모르게 중립이다.
- 지정자: `MaxElements`(위 표)와 `Required`(원소가 `ObjectRef`·`SoftObjectRef`인 배열만, 원소마다 non-null).
- 순환·깊이: struct 원소 배열 멤버는 원소 깊이 + 1이고(RMC2303), 배열을 거친 자기 참조(`StructRefArray<Self>`)는 RMC2302다. 모듈 등록·생성 순서도 원소 struct를 먼저 둔다(`StructRefArray<S>::Trace`가 `GeneratedType<S>::Trace`를 인스턴스화하므로 생성 헤더 안에서 원소 struct의 특수화가 먼저 나온다).
- 형질: `ContainsReferenceArray`는 생성 `static constexpr`이고 descriptor struct 항목 `flags`의 `ContainsReferenceArray`다(쓰기 순서 Atomic → ContainsStrongReference → RequiresWorldOnlyOwner → ContainsReferenceArray — 선언 플래그 Atomic이 생성 형질보다 앞이고, 클래스 항목의 Atomic은 RMC1000). RMC2206은 원소 struct까지, RMC2006은 private 원소 enum·struct까지 본다.

생성 코드: 배열 필드 스키마는 `{id, FieldKind::X, flags, name, {}, {ValueKind::Array, Guid{}}, category, displayName, toolName, {ValueKind::E, 원소 대상 ID}, maxElements}`다(`FieldKind::ObjectReference`는 `ObjectRefArray`만). 클래스 함수표는 `read`·`write`·`writeStaged` 없이 codec과, 원소가 Enum·Struct면 그 native 키(`valueNativeType` — 아니면 `nullptr`), 경로 접근 thunk(`path`)를 싣는다(통째 쓰기는 Unsupported — 원소 단위 접근은 아래 "경로 접근"의 생성 path thunk). 클래스와 참조 struct의 생성 Trace는 상속한 import 필드를 포함한 **모든** 배열 필드·멤버에 `detail::TraceArrayField(value.*(&::Owner::f), visitor)`를 부른다 — 실제 C++ 타입이 오버로드를 고르므로(`Array<E>`는 아무것도 하지 않는다) descriptor에서 컨테이너를 잘못 추론해도 Trace가 빠지지 않는다. 생성 `static_assert`(전체 특수화 안): `Array<S>`는 `!GeneratedType<S>::ContainsStrongReference`, `StructRefArray<S>`는 `ContainsStrongReference && !ContainsReferenceArray`, 원소 struct의 `Depth < Depth`와 형질 포함, WorldOnly가 아닌 클래스의 원소 struct `!RequiresWorldOnlyOwner`, struct 특수화의 PROPERTY 멤버마다 `ContainsStrongReference || !IsNativeStrongReference<decltype(S::m)>`와 `ContainsReferenceArray || !detail::RegistryContainer<decltype(S::m)>::value`(Binds 미러는 생략 — `VerifyBinds`가 실제 타입의 멤버 형식을 미러 필드 형식과 `IsSameV`으로 대조하고 모델 형질은 그 형식으로만 계산되므로 같은 보장이다), struct 멤버마다 `ContainsReferenceArray || !GeneratedType<M>::ContainsReferenceArray` — 모델 형질을 C++ 멤버 타입과 직접 대조한다(encode 고정이 `ContainsReferenceArray`에 기대므로 틀린 모델 값은 컴파일 오류다). struct 특수화를 내는 생성 헤더는 `<Object/NativeMemberTraits.h>`를, 다른 헤더의 원소 enum·struct(상속 import 필드의 원소 포함)는 그 생성 헤더를 include한다.

descriptor 필드 항목: 배열이면 `"value": {"kind": "Array"}`(type 없음), `"element": {"kind": …, "type": …}`(type은 원소가 참조·ClassRef·Enum·Struct일 때만 — Enum·Struct·ClassRef는 0이 아닌 ID, 참조는 ""(Object)도 된다), `"maxElements"`(1..1,048,576)이고, 배열이 아니면 `"element": null`·`"maxElements": 0`이다(Optional·Map·Set은 위 B9e·B9f2 문단 — Map은 키 형식 `"key": {"kind": …, "type": …}`, 그 밖의 필드는 `"key": null`이다). schema는 그 뒤 BS8d3에서 2로 올랐다(클래스 flags `Exported`)이고, 배열 컨테이너 종류(Array·ObjectRefArray·StructRefArray)를 적는 칸은 없다(원소 종류와 원소 struct 형질로 정해진다 — Map 키 형식 칸 `key`와 다른 이야기다). import는 이 규칙과 `fieldKind`(원소 ObjectReference ⇔ ObjectReference)·`Required`(참조 원소)의 대응을 엄격히 읽어(RMC1000) 원소 대상을 이은 뒤 종류(RMC1000)·존재(RMC2003)·private(RMC2006)를 검사하고, struct `flags`의 `ContainsReferenceArray`를 필드·멤버 flag와 대조하며, StructRefArray 원소 struct(원소 struct가 참조 struct)가 `ContainsReferenceArray`면 RMC1000이다(descriptor flags만 비교하므로 import 순서에 의존하지 않는다). import 필드는 원소 정보와 `maxElements`를 보존한다(파생 클래스 스키마가 상속 `maxElements`를 그대로 내야 Registry 상속 대조를 통과한다). 저장·복원·한도는 [Serialization](Serialization.ko.md)의 "리플렉션 배열 값"이다.

## 생성물과 등록

생성 헤더는 `GeneratedType<T>`의 `Id`, `Schema`, `Ops`, `Register`를 제공한다. `Schema()`는 타입·필드 ID, 버전, 이름, 값 형식(`ValueType`), 기반 타입 ID와 참조 대상 타입 ID를 제공한다. `Ops()`는 실제 C++ 타입으로 검증되는 read/write/writeStaged 함수, 그래프 저장·복원 codec(encode/decode — `Object/GeneratedFieldCodec.h`), Struct·Array 필드와 struct 멤버의 경로 접근 thunk(`Object/GeneratedPathAccess.h` — 아래 "경로 접근")와 자동 Trace를 제공한다. 클래스 특수화의 자동 Trace는 static `Trace(const T&, TraceVisitor&)`이고 `ops.trace`가 감싼다(리플렉션 base를 둔 native 파생이 부른다 — 아래 "native 클래스").

모듈마다 모듈 생성 헤더 `<M>/<M>.module.generated.h`가 생긴다. 이 헤더는 `rocks::generated::<M>::RegisterModuleTypes`의 선언만 담는다(include는 `Object/ModuleRegistration.h`뿐이다). 정의는 빌드가 그 모듈의 번역 단위로 한 번 컴파일하는 모듈 생성 소스(`<out>/Generated/<M>/Source/<M>.module.generated.cpp`)에 있으므로 호출하는 쪽은 M에 의존(링크)해야 한다. 타입별 생성 헤더를 끌어오지 않으므로 `GeneratedType<T>`·`TypeIdOf<T>`를 쓰는 코드는 그 타입의 생성 헤더(`<M/Type.generated.h>`)를 따로 include한다. 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다 — 등록은 .cpp에서 한다(정책 검사 `module-generated-header-include`, [리플렉션 생성 소스](../Design/ReflectionGeneratedSources.ko.md) 8절). 이 함수는 registration마다 모듈을 한 번만 등록하고(의존 그래프의 여러 경로로 다시 불리면 바로 성공한다), import한 의존 모듈의 `RegisterModuleTypes`를 먼저 부른 뒤 이 모듈의 public 타입을 base 먼저 등록한다. 같은 ID가 같은 native 타입으로 이미 등록되어 있으면 새로 등록하지 않고 그 타입의 lease를 붙잡으므로, 여러 모듈이 같은 의존 모듈을 공유해도 한 번만 등록되고 공유 타입을 쓰는 쪽이 있는 동안에는 그 타입이 해제되지 않는다. 실패하면 그 호출의 기록을 역순으로 되돌리고 오류를 돌려준다. private 헤더가 있는 모듈은 `<M>/<M>.private.module.generated.h`의 `RegisterPrivateModuleTypes`가 public 타입(public 헤더가 없으면 의존 모듈)까지 등록한 뒤 private 타입을 등록한다. 이 헤더는 그 모듈의 private include에서만 보인다. 생성 코드의 이름은 모두 `::rocks::`로 한정하므로 모듈 이름이 rocks의 이름을 가리지 않는다.

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

## native 클래스(`ROCKS_NATIVE_TYPE`, `Object/NativeClass.h`)

MetaCompiler를 거치지 않는 Object 파생 클래스는 `ROCKS_NATIVE_TYPE(Type, Base, IdHigh, IdLow)`로 선언하고 `RegisterNativeClass<T>(types, module)`로 등록한다. .cpp 안의 시험 타입처럼 Metadata 입력이 될 수 없거나, 영속 상태가 없는 런타임 전용 파생에 쓴다. native 클래스가 더한 필드는 리플렉션·저장 대상이 아니며, 강한 간선은 `void Trace(TraceVisitor&) const noexcept`로 방문한다. 등록이 기반부터 각 클래스가 직접 선언한 Trace를 차례로 부르므로 `Super::Trace`는 부르지 않는다. Trace의 접근 지정은 자유다 — 매크로가 접근자 `detail::NativeClassAccess`를 friend로 선언하므로 private·protected Trace도 찾고 부른다(공개 이름만 보던 이전 탐지는 접근할 수 없는 Trace를 조용히 건너뛰었다). Trace라는 이름을 직접 선언했으면 정확히 이 시그니처여야 하고, const·noexcept 누락·다른 매개변수·static 함수·데이터 멤버·오버로드·템플릿은 `RegisterNativeClass`의 static_assert가 사슬 전체(기반 native 클래스 포함)에서 거부한다(물려받은 Trace는 그 기반 단계가 부른다). native 판정(`NativeClassType`)은 T 자신의 매크로 멤버(`ThisClass`가 T)로 하므로 생성 헤더 include 여부와 무관하게 번역 단위마다 같다.

기반은 `rocks::Object`, 다른 native 클래스 또는 리플렉션 클래스(`ROCKS_CLASS` — 그 타입의 생성 헤더를 include한다)다([리플렉션·빌드 보강](../Design/ReflectionFoundations.ko.md) 9.1절 B8a). native 사슬 위에 리플렉션 조상 R(`detail::ReflectedAncestor<T>` — Super 사슬을 매크로 멤버로만 올라가 처음 만나는 native가 아닌 조상, 사슬이 Object에서 끝나면 void)이 있으면:

- 스키마는 `GeneratedType<R>::Schema().fields`(R이 상속한 필드 포함)를 같은 ID로 담고 함수표는 `GeneratedType<R>::Ops().fields`를 빌린다. R의 thunk는 `Resolve<R>`·`construction.Get<R>`의 `IsNativeA` 판정이라 파생 객체에 맞으므로 base 필드의 읽기·쓰기·경로 접근(`PathRead`·`PathWrite` 등, base struct 필드의 멤버 포함)이 파생 객체에 그대로 된다. enum·struct 필드는 같은 등록 레코드를 의존으로 붙잡는다(R의 의존 타입이 먼저 등록되어 있어야 한다 — `RegisterModuleTypes`).
- `TypeFlags::WorldOnly`만 R에서 물려받는다(생성 스키마의 flags는 상속한 WorldOnly를 포함한다). WorldOnly가 아닌 R 위의 native는 flags 0이고, Abstract(선언 정책·C++ 추상) R 위의 구체 native는 Abstract 비트를 물려받지 않아 생성할 수 있다. WorldOnly base의 파생을 수동 함수표로 등록하며 플래그를 빠뜨리면 InvalidArgument, 상속 필드를 담지 않으면 TypeMismatch다. R의 enum·struct 필드가 가리키는 타입이 등록되지 않았으면 base가 있어도 NotFound다.
- GC Trace는 native 사슬의 손 Trace를 부르다가 R에서 생성 `GeneratedType<R>::Trace`(상속 필드 포함)를 부르고 멈춘다. ROCKS_CLASS는 손 Trace(RMC2000)와 강한 참조 NATIVE 멤버(RMC2205)를 금지하므로 이중 방문·누락이 없다.
- `completeFieldLayout = false`·`parallelTraceSafe = false`다. native 파생의 C++ 상태를 모르므로 그 객체를 루트로 하거나 그 객체에 닿는 객체 그래프 저장과, 그 타입 노드의 로드는 Unsupported다(필드·codec은 모두 있다 — 같은 C++ 타입을 레이아웃 선언만 켜서 등록하면 저장·로드된다. GC는 Trace만 쓴다).
- 수명: 빌린 thunk와 생성 Trace는 `GeneratedType<R>`의 inline 함수라 `RegisterNativeClass<T>`를 인스턴스화한 번역 단위에서 인스턴스화되므로 그 코드 수명은 T의 module lease(`RegisterNativeClass`의 `module`)가 지키고, base 스키마와 enum·struct 의존 레코드는 base 레코드의 dependents가 지킨다. native 파생이 등록되어 있으면 base의 Unregister(그리고 그 base를 등록한 `ModuleRegistration::Unregister`)는 Busy이므로 native 토큰을 먼저 해제한다.

```cpp
#include <Game/Pawn.generated.h>   // 리플렉션 기반 game::Pawn(ROCKS_CLASS)의 생성 헤더
#include <Object/NativeClass.h>

class SpinningPawn final : public game::Pawn {
    ROCKS_NATIVE_TYPE(SpinningPawn, game::Pawn, 0x5b1c7d2e8a4f4c10ull, 0x9e02b6a1d3c84f57ull)

public:
    using Pawn::Pawn;
    void Trace(rocks::TraceVisitor& visitor) const noexcept { visitor.Visit(target); }   // 자기 간선만
    rocks::ObjectRef<game::Pawn> target;
};

auto token = rocks::RegisterNativeClass<SpinningPawn>(types);   // 리플렉션 기반이 먼저 등록되어 있어야 한다
auto pawn = objects.NewObject<game::Pawn>(access, rocks::ObjectTypeIdOf<SpinningPawn>());
```

`ObjectTypeIdOf<T>()`는 native·리플렉션 타입을 함께 받는 정적 ID다: `NativeClassType<T>`면 `T::StaticTypeId`, 아니면 생성 ID(`GeneratedType<T>::Id()` — `TypeIdOf<T>()`와 같은 값)다. 둘 다 아니면(native 파생이 매크로를 빠뜨렸거나 생성 헤더를 include하지 않았으면) "incomplete type" 대신 두 경우를 안내하는 static_assert로 멈춘다. `NativeTypeIdOf<T>()`는 native 전용이다. 기반이 native도 리플렉션 클래스도 아니면(매크로를 빠뜨린 중간 native 클래스 포함) `ReflectedAncestor`의 static_assert가 생성 헤더 include를 안내한다.

MetaCompiler는 클래스 `GeneratedType<T>`마다 `static void Trace(const T& value, TraceVisitor& visitor) noexcept`(상속 필드를 포함한 모든 참조·struct·배열 필드 — 방문할 필드가 없으면 매개변수 이름 없는 빈 본문)를 내고 `ops.trace`는 `Trace(static_cast<const T&>(object), visitor)`로 그것을 감싼다.

MetaCompiler는 클래스·struct `GeneratedType<T>`마다 필드 방문 함수 `template <class Visitor> static constexpr void VisitFields(const T& value, Visitor& visitor) noexcept`도 낸다([로깅 설계](../Design/LoggingSystem.ko.md) 14.1.6절 LG16). 상속 필드를 포함해 선언 순서(가장 먼 base의 필드부터, 선언 타입 안에서는 소스 순서 — 스키마 필드 배열의 안정 ID 순과 다르다)대로 `visitor.Field("멤버 이름", 값)`을 부른다. 값은 Trace와 같은 철자의 멤버 포인터 직접 읽기이고, 자기 bitfield bool은 헤더 안의 직접 읽기(`Get_` 정의는 생성 소스에 있어 다른 모듈에서 부르지 않게 한다), 상속한 bitfield bool은 선언 타입으로 바꿔 헤더 안에서 직접 읽는다(`ROCKS_GENERATED_BODY`가 모든 `GeneratedType`을 friend로 둔다). 필드가 없으면 매개변수 이름 없는 빈 본문이다. registry·할당이 없으므로 로그 구조체 펼치기(`Object/LogStructure.h`의 `LogStructure<T>` 일반 특수화)가 생산자 경로에서 쓴다. 등록·함수표가 없는 템플릿이라 쓰지 않는 번역 단위에서는 인스턴스화되지 않는다. 생성 헤더는 `Object/LogStructure.h`를 include하지 않는다. 일반 특수화의 선언은 생성 헤더가 include하는 `Object/GeneratedFieldAccess.h`에 있고, 정의는 `Object/LogStructure.h`에 있다. 리플렉션 타입을 로그 인자로 쓰는 번역 단위가 정의 헤더를 직접 include하며, 빠뜨리면 `static_assert`로 거부한다(로깅 설계 14.1.6절 L30). 시험은 MetaTests `Meta.VisitFieldsReadsMembersDirectly`(생성 모양·private·상속·bitfield·빈 struct와 일반 특수화 인스턴스화), `Meta.LogStructureDeclarationWithoutDefinitionHeader`(정의 헤더 없이 판정이 같고 쓰면 거부)와 ObjectRuntime `LogStructureReflectionTests`다.

native 클래스 안에는 `ROCKS_PROPERTY`·`ROCKS_NATIVE`를 쓰지 않는다. MetaCompiler가 보지 않으므로 attribute로만 남고 GC가 추적하지도 저장하지도 않는 필드가 된다. Policy(`Rocks.ToolTests policy`·self `Policy.RepositoryReflectionAnnotations`)가 엔진 저장소(템플릿 포함)의 소유 native 소스(.h·.c·.cpp·.mm — 시험·Developer 포함)에서 다음을 거부한다.

- `reflection-annotation-native-type`: `ROCKS_NATIVE_TYPE`을 직접 담은 클래스 본문 안의 두 annotation(중첩 클래스의 멤버는 그 중첩 클래스로 판정). 리플렉션 헤더 안이면 MetaCompiler도 거부하므로 이 규칙의 실익은 .cpp 안 native 클래스에 구체적인 메시지를 주는 데 있다.
- `reflection-annotation-unscanned`: 빌드가 리플렉션 헤더로 찾지 않는 파일의 두 annotation — 표지(`ROCKS_CLASS(`·`ROCKS_STRUCT(`·`ROCKS_ENUM(` 줄 첫머리)가 없는 파일, `.h`가 아닌 파일, 모듈 시험(`Tests/`), 모듈 소스 폴더(`Private/`). 표지 판정은 빌드와 같은 `ReflectionHeaderScan.HasMarkup`이고 위치는 경로 성분으로 보는 lexical 근사다.

주석·문자열과 전처리 지시문 줄(매크로 정의)은 보지 않고, 한 위치는 native-type을 먼저 보고한다. 알려진 lexical 한계: 매크로로 감싼 annotation(정의 줄은 보지 않고 사용 위치에는 이름이 없다), `u8`·`L` 접두 문자 리터럴 안의 중괄호, 줄 끝 역슬래시 앞에 블록 주석이 있는 `#define` 이어짐 줄, `#if`로 갈린 두 클래스 머리(중괄호 짝이 어긋난다)는 판정이 틀릴 수 있고, `.hpp`·`.inl` 같은 다른 확장자는 검사하지 않는다.

## World 타입(B8b)

World 모듈의 Level·Actor·Component는 `ROCKS_CLASS(WorldOnly)`, SceneComponent는 `ROCKS_CLASS()`(WorldOnly 상속)인 리플렉션 클래스다([리플렉션·빌드 보강](../Design/ReflectionFoundations.ko.md) 9.1절, 멤버 분류는 [엔진 런타임](../Design/EngineRuntime/04-ObjectSystem.ko.md) 4.6절). `World.Build.cs`는 `PublicDependencies = ["ObjectRuntime", "CoreTypeBindings"]`(SceneComponent의 Transform 필드)와 리터럴 `Metadata`(네 헤더)를 두고, identity 명세는 `World.rocksids.json`(타입 4·필드 7 — 이전 수동 TypeId는 어디에도 영속되지 않았으므로 ids 도구로 새로 발급했다)이다.

- 영속 필드는 모두 `ReadOnly`이고 도구 이름(`Name`)을 가진다: Level `actors`·`name`, Actor `components`·`rootComponent`(`ObjectRef<SceneComponent>` — 영속 강한 참조)·`enabled`, Component `enabled`, SceneComponent `relativeTransform`. World 포인터·소유·Tick·수명·계층은 `ROCKS_NATIVE`다. 경로·리플렉션 쓰기는 ReadOnly로 Unsupported(`PropertyPathError::ReadOnly`)이고 로드의 staged 쓰기는 된다.
- 등록: `RegisterWorldTypes(ModuleRegistration&)`가 `rocks::generated::World::RegisterModuleTypes`를 부른다(멱등). Engine은 ModuleRegistration을 소유하고 World → `EngineConfig::registerModuleTypes` → `EngineConfig::registerTypes`(native) 순서로 등록하며, 종료할 때 타입을 Unregister하지 않는다(ModuleRegistration 파괴 → `TypeRegistry::Shutdown`).
- 생성 헤더 규약: `World/World.h`(그리고 `Engine/Engine.h`)는 생성 헤더를 include하지 않는다. 리플렉션 World 타입을 `SpawnActor<T>`·`AddComponent<T>`·`ObjectTypeIdOf<T>`의 인자로 쓰거나 `RegisterNativeClass<T>`로 World 타입의 native 파생을 등록하는 TU는 그 타입의 생성 헤더 `<World/<Type>.generated.h>`를 include한다(모듈 생성 헤더 `<World/World.module.generated.h>`는 등록 함수의 선언만 담는다). 모듈 이름 `World`와 클래스 `rocks::World`가 겹치지만 등록 함수는 `rocks::generated::World` 이름공간이고 생성 코드는 이름을 `::rocks::`로 한정하므로 서로 가리지 않는다.
- 다른 모듈에서 World 타입을 상속한 리플렉션 클래스(`WorldTestFixtures`의 `SaveActor : Actor`·`SaveComponent : SceneComponent`)는 World descriptor를 import해 상속 필드(`components`의 ObjectRefArray Trace 포함)를 생성 코드에 담는다.
- Level 그래프 저장은 구조 시험용이고(계층 비저장, native 파생이 있으면 Unsupported), 로드한 객체는 World 장부 밖이다(계층·World API InvalidArgument — 편입은 레벨 패키지 단계).

## 필드 접근과 실패 보장

`FieldValue`는 이동 전용 소유 값이며 String도 자체 복제한다. kind에 대응하는 payload만 유효한 값으로 읽는다. 같은 FieldValue의 동시 변경이나 allocator callback에서 그 래퍼로 변경 재진입하는 동작은 금지한다. allocator는 값과 진행 중인 호출보다 오래 살아야 한다.

일반 read와 쓰기 가능 필드의 write는 ObjectAccessContext와 ObjectHandle을 받아 Registry에서 owner thread, context, generation, 논리 수명과 owner의 native type을 검사한다. 참조 필드 값은 선언 타입 또는 그 파생 객체여야 한다. raw object pointer나 StringView를 외부 반환값으로 유출하지 않는다. staged write는 ConstructionContext의 아직 게시되지 않은 객체를 검사한다.

- String read는 임시 소유 값에 복제하고, 할당 callback 이후 Borrow의 논리 유효성을 다시 확인한 뒤 output에 게시한다. OOM 또는 논리 파괴는 기존 output을 유지한다.
- String write는 대상 필드의 allocator로 replacement를 완성하고, 논리 유효성을 다시 검사한 후 할당·해제 없는 Swap으로 commit한다. OOM과 commit 전 논리 파괴는 기존 필드를 유지한다.
- Swap 이후 이전 문자열의 storage 반환 callback에서 객체를 논리 파괴해도 이미 완료한 commit은 성공이다. 일반 write의 Borrow는 이전 storage 정리까지 객체의 물리 수명을 보존한다. 이때 추가 객체 접근을 하지 않는다.
- 정수 폭 축소는 범위를 확인하고 `Overflow`를 반환한다. 타입이 다르면 `TypeMismatch`이며 필드는 변경하지 않는다.
- ObjectRef write는 `ObjectRegistry::SetReference` 또는 `ConstructionContext::SetReference`를 통과한다. owner/target 검증, typed target 규칙, GC barrier를 raw store로 우회하지 않는다.

등록된 `TypeLease::Ops()`의 모든 일반 write는 실제 대상 객체의 타입 lease를 조회하고 native 타입, 필드 ID와 값 형식을 검증한다. 대상 타입에 필드가 없으면 `NotFound`, native 타입·필드 형식·전달 값 형식이 다르면 `TypeMismatch`를 반환한다. 오류 우선순위는 context·수명, 타입·필드, 전달 값 종류, 읽기 전용 정책, 값 내용(정수 폭 `Overflow`, 소프트 참조의 null 규칙·경로 상한 `InvalidArgument`) 순서다. 따라서 같은 native 타입의 다른 TypeId 별명이나 별도 TypeRegistry의 객체도 실제 대상 schema의 필드 정책을 검사한다.

`ReadOnly`는 등록된 함수표의 일반 write에 적용되는 편집 정책이다. 원본 또는 대상 schema 중 하나라도 해당 필드를 ReadOnly로 선언하면 `Unsupported`를 반환한다. 미게시 객체에 대한 `writeStaged`는 초기화와 복원을 위해 허용한다. 등록된 write마다 타입 lease 조회와 필드 검색 비용이 발생하며, 안전한 정책 확인을 우선한다. `write`는 callable 값이며 기존 `.write(...)` 호출 구문을 유지한다. 같은 C++ 타입을 다른 TypeId로 등록하는 별칭 스키마(생성 스키마의 복사본)는 `TypeSchema::toolName`을 비워야 한다(타입 도구 이름은 Registry 안에서 유일하다 — 원본과 같은 이름이면 AlreadyExists, 리플렉션 B9d2). 이 값의 callback 코드 수명은 `TypeLease`가 보유하는 `ModuleLease`에 종속된다.
ObjectReference 필드는 등록 시점에 참조 대상 타입이 아직 등록되지 않을 수 있으므로 write 시점에 원본 및 대상 TypeRegistry에서 각각 선언 타입을 조회한다. 둘 다 구체 타입이면 native 타입이 같아야 하며, 서로 다른 TypeId가 같은 native 타입을 가리키는 별칭은 허용한다. 한쪽이 0 GUID이면 일반 Object로 취급하고 다른 쪽의 구체 타입 제약을 적용한다. 실제 참조가 비어 있지 않으면 두 구체 제약에 모두 맞아야 한다. 두 구체 선언이 서로 충돌하거나 타입을 조회하지 못하면 null 대입도 거부한다. 정상적인 자기 참조 선언의 null은 허용하며 부분적으로만 비어 있는 잘못된 핸들은 거부한다.

이 정책은 C++ 멤버에 대한 직접 접근이나 등록 전 `GeneratedType<T>::Ops()`의 원본 callback 호출을 차단하는 보안 경계가 아니다. 수동 코드 제공자는 원본 callback과 필드 접근의 사용 범위를 관리해야 한다.
`String::Swap`은 allocator와 storage 소유권도 함께 교환한다. 양쪽 allocator가 각 저장소의 최종 반환보다 오래 살아야 한다. 같은 String의 변경 재진입은 기존 String 계약대로 금지한다.

등록된 read(생성 `detail::ReadGeneratedField`)는 복사를 owner의 encode 고정(`PinForEncode`) 아래에서 한다. String·Soft 경로 복사는 새 버퍼를 할당한 뒤 원본을 읽으므로, 그 할당 콜백이 같은 필드를 쓰면(`WriteGeneratedField` — `ResolveForWrite`) 고정 때문에 Busy가 되어 원본이 복사 전에 해제되지 않는다. 생성 write(`WriteGeneratedField`)도 String·Soft는 쓰기용 Borrow 뒤 새 값 할당부터 Swap까지 같은 고정을 쥐어, 할당 콜백의 재진입 생성 write가 Busy가 되고 바깥 Swap이 그 갱신을 덮어쓰지 않는다. 고정은 게시 또는 실패 직후 풀리고 옛 버퍼·실패한 새 버퍼는 그 뒤에 반환된다(B6a에서 고친 결함). 수동 함수표의 write callback에는 이 고정이 없으므로 할당하는 수동 callback은 같은 규칙을 스스로 지킨다. 단일 값 변환 규칙은 `detail::CopyTargetValue`·`WriteTargetValue` 한 벌이고 `CopyFieldValue`·`WriteScalarField`는 `FieldValueKind<Member>()`로 위임한다. 참조 값의 비정규 null(registry 0 또는 generation 0인 non-zero handle)은 모든 쓰기에서 `ObjectHandle{}`로 정규화한다: `FieldWriteOperation`은 null로 검증하고 callback에 reference를 `ObjectHandle{}`로 바꾼 사본(할당 없음)을 넘기며, `detail::ValidateFieldWrite`의 null 판정도 `IsNull()`이다.

## 경로 접근(`Object/PropertyPath.h`)

```cpp
PropertyPathBuffer path;
PropertyPathFailure failure;
if (auto parsed = rocks::ParsePropertyPath(meshType, "slots[2].material", path, &failure); !parsed) {
    // failure.reason == PropertyPathError::UnknownProperty면 AS 4.6절 UnknownProperty 진단(step·offset)
}

FieldValue previous(undoAllocator);
auto written = rocks::PathWrite(registry, access, mesh, path.View(), value, &previous, &failure);
```

경로는 객체 하나 안의 FieldId 체인 + 원소 index·Map key다([리플렉션·빌드 보강](../Design/ReflectionFoundations.ko.md) 7·7.1절, key 단계는 [리플렉션 B9](../Design/ReflectionExtensions.ko.md) 1.5·7.3절). `PropertyPath{Span<const PathStep> steps, Span<const PathKey> keys}`(MetaSchema)이며 최대 `MaxPropertyPathSteps`(17 = 최상위 필드 + struct 중첩 16) 단계, 원소 index는 배열 필드 단계에 두고(`[slots[2], material]` — `PathSelector::Index`) Map 항목·Set 원소는 key 단계(`PathSelector::Key` — index는 keys 위치, `PathKey`는 키 종류와 wire 정규형)다. 정규형(selector None이면 `index == 0`, Key면 `index < keys 수`)이 아니면 연산이 InvalidArgument로 거부한다 — 같은 위치가 한 표현만 가지므로 `SamePropertyPath`(Key 단계는 키 값 — Name은 Core Name 규칙)로 Undo 기록을 병합할 수 있다. 참조를 따라 다른 객체로 들어가지 않는다.

- 문자열 앞단: `ParsePropertyPath(lease, text, out, failure, hook)`는 `name ('.' name | '[' index ']' | '[' key ']')*`를 클래스·struct lease 스키마로 해석한다(key — Map·Set 필드 뒤 `[`는 키 종류의 텍스트, 필요할 때만 큰따옴표, escape `\\`·`\"`·`\n`·`\r`·`\t`, Soft·ClassRef 키는 hook `PathKeyTextHook` — 리플렉션 B9f3). 이름은 ASCII 식별자이고 필드의 실효 도구 이름(`EffectiveToolName` — toolName, 없으면 C++ 이름. toolName이 있으면 C++ 이름은 받지 않는다)과 `EqualsIgnoringAsciiCase`로 비교하며 Opaque 필드는 찾지 않는다. index는 정규 10진(`0` 하나 또는 0이 아닌 숫자로 시작, uint32 범위)이다. 할당하지 않고 스레드와 무관하며, 실패하면 out은 그대로다. 파생 스키마가 상속 필드를 같은 ID로 담으므로 base lease로 해석한 경로를 파생 객체에 쓸 수 있다. `FormatPropertyPath(lease, path, out)`는 실효 도구 이름으로 되돌린다(out의 allocator로 한 번 할당 — Parse와 왕복).
- 연산(소유 스레드 전용): `PathRead`, `PathWrite`(배열 원소 교체·Map 항목 넣기·교체 포함, 옛 값 `previous`), `PathCount`, `PathInsertElement`(마지막 단계 index = 삽입 위치, 길이면 끝), `PathRemoveElement`(지운 단일 값 원소 `removed`), `PathClear`, Map·Set key 단계의 `PathContains`·`PathInsertKey`·`PathEraseKey`(리플렉션 B9f3 — `PathCount`·`PathClear`는 B9f3에서 `PathArrayLength`·`PathClearArray`의 이름을 바꿔 Map·Set도 받는다). 끝 지점은 최상위 단일 값 필드가 등록된 read·write(수동 함수표·동적 필드도 같다), struct 멤버 단일 값과 `Array<E>` 원소가 경로 thunk, 참조 멤버가 `SetReference`(StructRefArray 원소 안이면 원소 위치 오버로드 `ObjectRegistry::SetElementReference(access, owner, const detail::StructElementLocation&, ObjectRef<T>&, target)` — 위조한 위치를 공개 API로 받지 않도록 private이고 경로 구현의 `detail::SetPathMemberReference`만 friend다), ObjectRefArray 원소가 `SetReferenceAt`·`InsertReferenceAt`·`RemoveReferenceAt`·`ClearReferences`, StructRefArray가 `InsertElementAt`·`RemoveElementAt`·`ClearElements`(원소는 값 없이 기본 원소, 원소 멤버는 멤버 단위)다. `Array<S>` 원소 삽입도 값 없이 기본 원소(`S(Allocator&)` 또는 `S()`)이고, 단일 값 원소 삽입은 값이 필수다. struct·배열을 통째로 읽거나 쓰면 Unsupported(`AggregateValue`)이고 Resize·원소 이동·복제는 없다. Optional 필드(리플렉션 B9e)는 단일 값 내부면 FieldValue(`kind` = 내부 종류, `present`)로 읽고 쓰고(`present = false`가 unset), 값 struct 내부면 `PathEmplaceOptional`(기본 값 — 이미 set이면 기본 값으로 바꾼다)·`PathResetOptional`로 만들고 비우고 `PathIsOptionalSet`으로 set 여부를 보며(멤버 없는 struct도) 멤버 경로(`opt.member`)는 set일 때만 닿는다(unset이면 NotFound — 사유 `OptionalUnset`, 끝이 Optional<값 struct>가 아니면 TypeMismatch — 사유 `NotOptional`). 빈 자리의 새 값은 `ObjectRegistry::ObjectAllocator()`로 만든다. Map·Set 필드(리플렉션 B9f3)는 key 단계(`perPlatform[Android]` — `PathSelector::Key`와 `PropertyPath::keys`의 키 wire 정규형)로 항목을 가리키고, 규칙·오류는 [리플렉션 B9](../Design/ReflectionExtensions.ko.md) 7.3절 "구현(B9f3)에서 정한 점"이다.
- 실패 사유: 모든 연산과 Parse가 `PropertyPathFailure{reason, step, offset}`을 채운다. `UnknownProperty`(문자열의 모르는 이름, NotFound)와 `UnknownField`(FieldId 체인의 없는 필드 — 스키마가 바뀐 옛 경로, NotFound)를 구별하고, `NotArray`·`NotStruct`·`StaticTypeMismatch`·`NotOptional`(TypeMismatch — `NotOptional`은 Optional<값 struct> 연산의 끝이 그 필드가 아님, 리플렉션 B9e), `OptionalUnset`(unset Optional<struct>의 멤버로 들어감, NotFound), `IndexRequired`·`IndexNotAllowed`·`IndexOutOfRange`·`InvalidValue`(InvalidArgument), `ReadOnly`·`AggregateValue`·`NoPathAccess`(Unsupported), `TooManyElements`(CapacityExceeded), `Syntax`·`TooDeep`(InvalidArgument)이다. 문맥·수명·Busy·OutOfMemory·Overflow·Registry 대상 검사(StaleHandle 등)·게시 직전 재확인, 단계 0개·정규형 위반은 `None`이고, Unsupported는 유효 플래그에 ReadOnly가 있을 때만 `ReadOnly`다(수동 callback의 그 밖 Unsupported는 `None`). `NoPathAccess`는 Opaque 필드로 끝나는 읽기·쓰기와 previous를 요청했는데 최상위 read가 없는 경우도 뜻한다. step은 문맥·수명과 Borrow 획득 오류면 0, 17단계 초과면 17이고, 고정·게시 직전 재확인·Registry 호출 오류는 그 연산을 하던 단계다. 문자열 앞단의 끝 `.`은 struct 뒤면 Syntax(이름 없음), struct가 아닌 값 뒤면 NotStruct이고, `TooDeep`은 17단계 뒤의 `.`에서 판정한다.
- 검증: 바깥 struct·배열 필드의 플래그를 `PropagateFieldFlags`로 누적해 ReadOnly가 멤버·원소 쓰기와 삽입·삭제·비우기를 모두 막는다(EditorOnly·Transient는 막지 않는다). 값 쓰기·단일 값 원소 삽입은 `detail::ValidateFieldWrite`(원소 모드 포함), 구조 변경은 새 `detail::ValidateArrayEdit(target, ArrayEdit)`(배열이 아니면 TypeMismatch, ReadOnly면 Unsupported)를 지나고, 삽입 뒤 길이는 `detail::ValidateArrayLength`(필드 MaxElements — CapacityExceeded)다. 참조 값은 `FieldWriteOperation`과 같은 순서로 기대 타입(`AcquireRegisteredType`)과 non-null 대상(`AcquireType`) lease를 얻어 완료까지 쥔다. Registry 컨테이너(ObjectRefArray·StructRefArray)는 MaxElements를 검사한 뒤 자체 재판단 규칙으로 할당하므로 할당 콜백의 재진입이 겹친 수만큼 상한을 넘길 수 있다 — 넘친 배열은 저장에서 `ArrayTooLong`으로 드러난다(허용).
- 원자성: 쓰기는 `ResolveForWrite`로 Borrow를 얻는다(encode 고정 중이면 Busy, DestroyRequested owner는 Destroyed — 정리 중 제거·비우기는 Registry API를 직접 쓴다). 경로가 직접 할당하는 끝 지점(struct 멤버·배열 원소의 String·Soft 쓰기, `Array<E>` 삽입, 다른 allocator로의 옛 값 복사)만 할당부터 게시까지 owner를 encode 고정해 할당 콜백의 재진입 쓰기·Registry 컨테이너 변경·SetEdge를 Busy로 막고(주소 재계산은 하지 않는다 — 같은 크기 블록을 다시 받는 ABA를 놓친다), 고정은 RequestDestroy를 막지 않으므로 게시 직전 `Borrow.Get()`으로 소유자를 다시 확인한다(Destroyed면 미게시). 고정은 게시 또는 실패 직후 풀리고, 옛 값·옛 버퍼와 실패한 연산의 임시 버퍼는 그 뒤에 반환한다. 최상위 단일 값의 등록된 write(생성 write는 String·Soft 할당 동안 스스로 고정한다)와 Registry 컨테이너 연산은 자기 규칙으로 하고 경로가 고정을 쥐지 않는다. struct 원소 배열은 연산과 무관하게 그 단계 맨 앞에서 원소 struct를 정적 대조한다. 읽기는 Resolve + encode 고정 아래 복사하고 out 게시 전에 다시 확인한다(등록된 read 호출도 같은 고정으로 감싼다). `Array<E>` 삽입은 원소를 배열 allocator로 만들고, 용량이 모자라면 같은 allocator의 임시 배열에 Core 공개 성장 규칙 `ArrayGrowthCapacity`대로 예약한 뒤 소유자를 확인하고 원소를 무할당으로 옮겨 `Array<E> old(Move(live)); live = Move(temp);`로 게시한다 — 옛 버퍼의 해제 콜백은 게시된 새 배열만 본다(`live = Move(temp)`를 바로 쓰면 이동 대입이 옛 버퍼를 먼저 반환해 그 콜백이 해제 중인 배열을 본다). 삭제·비우기는 원소(비우기는 버퍼째)를 지역으로 옮겨 뺀 뒤 게시하고 게시 뒤 소멸한다(비우기는 용량까지 반환). 이것들은 **이동된 원소의 소멸자가 allocator를 부르지 않는다**는 리플렉션 struct·배열 원소의 일반 계약(NATIVE 멤버 포함)에 기댄다. 같은 배열을 C++ 코드로 직접 바꾸는 콜백은 계약 밖이고, 참조 struct는 struct 단위 원자성이 없다(멤버 하나의 쓰기만 원자적이다).
- 옛 값: `previous`·`removed`의 String·Soft는 그 FieldValue allocator가 원본과 같으면 옛 버퍼를 옮기고(추가 할당 없음), 다르면 고정 아래 게시 전에 그 allocator로 복사한다(Undo 기록이 객체 allocator의 버퍼를 쥐지 않게). 최상위 단일 값의 previous는 write 직전에 등록된 read로 얻는다 — 그 전에 참조 lease와 `ValidateFieldWrite`를 먼저 거치고(ReadOnly 등이 OutOfMemory보다 먼저), 쓰기용 Borrow로 고정 중이면 read 전에 Busy다. 실패하면 둘 다 바뀌지 않는다. struct 원소 제거에는 `removed`를 줄 수 없고(InvalidArgument) 연산 전 `PathRead`로 잎을 기록해 "기본 원소 삽입 + 잎 쓰기"로 재생한다(재생한 객체의 저장 byte가 같음을 시험으로 고정했다). 옛 참조 handle은 약한 참조다 — 간선을 지운 뒤에는 대상을 살리는 것이 없으므로 CU4가 RootRef로 붙잡기 전까지(이 연산의 해제 콜백 안 GC 포함) 옛 대상이 회수될 수 있어, 되살리기 전에 생존을 확인한다.

경로 thunk(`RuntimeFieldOps::path`·`RuntimeStructFieldOps::path`, 템플릿은 `Object/GeneratedPathAccess.h`)는 필드 하나에 하나이며 자기 단계만 처리하고 끝이 아니면 `detail::PathCursor::Descend`(공개 detail 헤더 `Object/PathCursor.h`, 구현 `Private/PropertyPathCursor.cpp`·`PropertyPathRunner.cpp`)로 다음 멤버의 thunk를 부른다. 대상 종류는 `ValueKind Target`으로 명시하고(requires 탐지 없이 C++ 형식과 컴파일 시 대조), 실행 시 `FieldTargetType(field).kind`·배열 여부·enum ID(`GeneratedType<E>::Id()`)·struct native 키(`MatchesStructView`와 같은 형태)를 등록 스키마와 대조해 `void*` 타입 혼동을 막는다(StaticTypeMismatch). 클래스 단일 값 필드의 `path`는 null이어야 하고(등록 InvalidArgument), 클래스 레코드는 스키마 위치 → 함수표 색인과 필드 ID 정렬 색인을 둔다(`TypeLease::FieldOps(schemaIndex)`·`FieldIndex(Guid)`).

생성 path thunk(B6b): MetaCompiler 생성 헤더는 `<Object/GeneratedPathAccess.h>`를 include하고 다음을 낸다. descriptor는 바뀌지 않는다(thunk의 대상 종류는 descriptor에 이미 있는 값 종류·원소 종류다).

```cpp
// 클래스 Struct·Array 필드 함수표: codec 뒤 valueNativeType(원소가 Enum·Struct가 아니면 nullptr을 명시), 그다음 path.
{FieldId, nullptr, {}, nullptr, /*encode*/ ..., /*decode*/ ..., nullptr,
 +[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept -> ::rocks::Result<void> {
     return ::rocks::detail::PathClassField<::Owner, ::rocks::ValueKind::UInt32>(cursor, &::DeclaringOwner::locked);
 }},
// struct 멤버(모든 멤버 — 단일 값 멤버도 경로가 이 thunk로 접근한다).
detail::StructFieldOps<&::S::m>(MemberId, &detail::PathStructMember<&::S::m, ValueKind::Utf8String>),
```

- `Owner`는 함수표를 내는 클래스(파생 — 함수표가 실제 타입 레코드에서 오므로 `static_cast<Owner*>`)이고 멤버 포인터는 선언 클래스의 것이다(다른 모듈에서 상속한 import 필드도 같다 — 이 모듈 헤더가 그 base 헤더를 include하므로 추가 include는 없다). struct 단계는 등록 `StructView`로 내려가므로 import struct의 멤버 thunk는 그 struct를 생성한 모듈의 것이 쓰인다.
- 대상 종류는 `FieldModel.TargetKind`(배열이면 원소 종류 — `ObjectRefArray`는 ObjectReference, `StructRefArray`·`Array<S>`는 Struct)다. 이 모듈 필드는 C++ 형식으로, 상속 import 필드는 base 모듈 descriptor의 종류로 만들고, `detail::TargetValueCheck` static_assert가 그 종류를 C++ 멤버 형식과 대조한다(아래). Binds 미러는 실제 타입(`NativeName`)의 멤버 포인터를 쓴다(`&::rocks::Vector3<float>::x`).
- 클래스 단일 값 필드에도 path를 낸다(리플렉션 B9i3c0 — Docs/Design/ReflectionExtensions.ko.md 10.2절). 게시된 객체의 최상위 단일 값 경로 읽기·쓰기는 그래도 등록된 read·write 한 경로이고, 단일 값의 thunk는 staged 객체의 경로 연산과 Detach·Attach가 쓴다. NATIVE 멤버는 스키마에 없으므로 thunk도 없다.
- 대상 종류가 C++ 멤버 형식(배열이면 원소 형식)과 다르면 `detail::TargetValueCheck` static_assert("path thunk target kind does not match the C++ member type")가 생성 헤더 컴파일을 실패시킨다 — MetaCompiler 검증 컴파일이 게시 전에 거부하고(문법상 유효하게 위조한 base descriptor의 상속 필드 종류 포함 — import 검사는 C++ 형식을 대조하지 않는다), 오래된 생성 헤더로 만든 thunk도 소비자 컴파일에서 잡힌다.

## 호스트 실행과 빌드 연결

다음 예시는 저장소 루트에서 실행한다.

```text
dotnet build Programs/Rocks.Tools.csproj
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll --module FoundationTemplate --input Templates/Game/Foundation/Source/GameObject.h --include GameObject.h --output-root out/Generated/FoundationTemplate --descriptor out/Metadata/FoundationTemplate.rocksmeta.json --identity-manifest Templates/Game/Foundation/FoundationTemplate.rocksids.json --compiler clang-cl
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll meta --root . --compiler clang-cl --metacompiler out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll --clang-arg=-DROCKS_BUILD_DEBUG=1 --clang-arg=-DROCKS_PLATFORM_WINDOWS=1 --clang-arg=-DROCKS_MONOLITHIC=1
```

Core 헤더를 컴파일하는 검사는 구성 매크로와 대상 OS 매크로(`ROCKS_PLATFORM_WINDOWS`·`ROCKS_PLATFORM_MAC`·`ROCKS_PLATFORM_ANDROID` 중 하나)를 함께 받는다. 빌드(`Rocks.Build.Cli`)는 `NativeConfigurations.BuildMacro`·`PlatformMacro`로 둘 다 넘긴다.

ToolTests `meta`는 시험마다 자기 임시 디렉터리에서 MetaCompiler·Clang 프로세스를 띄우므로 기본으로 CPU 수만큼 함께 돌린다(`--jobs N`으로 바꾼다). 결과 줄은 끝난 순서로, JSON 보고서는 이름 순서로 남는다. 프로세스 전역 상태(환경 변수 등)를 바꾸는 시험은 `exclusive`로 등록해 병렬 묶음이 끝난 뒤 하나씩 돈다. 새 meta 시험은 저장소 파일을 읽기만 하고 쓰기는 자기 임시 디렉터리에 한다.

MetaCompiler는 모듈 하나를 한 번에 생성한다. `--module`(모듈 이름), 짝을 이루는 `--input <헤더>`·`--include <철자>`(여러 번), private 헤더 표시 `--private <철자>`, 생성 루트 `--output-root`, descriptor `--descriptor <M>.rocksmeta.json`, 의존 모듈 descriptor `--import`(여러 번)를 받는다. 입력 헤더 전부를 include하는 번역 단위 하나를 Clang에 주므로 같은 모듈의 헤더 사이 base·참조가 풀린다. 생성 헤더는 `<생성 루트>/Public|Private/<철자의 .h를 .generated.h로 바꾼 경로>`에 쓴다. 위 예는 `out/Generated/SampleGame/Public/GameObject.generated.h`, 모듈 생성 헤더 `out/Generated/SampleGame/Public/SampleGame/SampleGame.module.generated.h`, 타입별 생성 소스 `out/Generated/SampleGame/Source/Public/GameObject.generated.cpp`, 모듈 생성 소스 `out/Generated/SampleGame/Source/SampleGame.module.generated.cpp`와 descriptor를 만든다. 타입별 생성 헤더는 `GeneratedType<T>` 특수화의 선언·컴파일 타임 형질·`Id()`·enum `IsDeclared()`·inline `Trace()`만 담고, 스키마·연산 표·등록·복사·검증·정적 검사 같은 본문은 타입별 생성 소스(`<생성 루트>/Source/Public|Private/<철자의 .h를 .generated.cpp로 바꾼 경로>`)가 한 번 정의한다. 그래서 `GeneratedType<T>::Schema()`·`Ops()`·`Register()` 등을 쓰는 코드는 그 타입의 모듈에 의존(링크)해야 한다. 생성 소스의 타입·필드 블록 앞에는 원본 선언을 가리키는 주석 `// <헤더 철자>:<줄> <타입>::<멤버>`가 있다. `--verify-sources`를 주면 생성 소스(타입별·모듈)도 게시 전에 구문 검사한다(빌드는 기본으로 주지 않는다 — 그 모듈의 compile action이 컴파일하고, 실패하면 빌드가 `RMC3000` 안내를 붙인다. 빌드 명령에 `--verify-metadata-sources on`을 주면 켠다). `--jobs <n>`은 게시 전 검증 컴파일을 함께 돌리는 수다(기본은 8과 프로세서 수 가운데 작은 값). 실제 빌드에서는 빌드 도구가 이 인자를 모두 구성한다.

컴파일러는 C++23, 예외·RTTI 비활성화와 경고 오류 정책을 적용한다. Windows SDK 환경은 공통 NativeEnvironment가 준비한다. macOS에서 위 예시의 `clang-cl`은 `clang++`로 바꾼다. annotation은 Clang 기능이며 지원하지 않는 컴파일러에서 빈 매크로로 대체하지 않는다.

`--include-dir`을 지정하면 전달한 순서만 사용한다. 빌드는 native 컴파일과 같은 순서 — 소유 모듈의 public include(끝에 생성 Public 루트), private include(끝에 생성 Private 루트), 의존 모듈의 public include(각 모듈의 생성 Public 루트 포함) — 로 넘기며, 생성 루트는 이번 실행의 stage 루트로 바꿔 넘긴다. 지정하지 않은 단독 호출에서는 저장소의 공개 헤더 경로, 저장소 루트, 입력 헤더의 디렉터리를 탐색한다. 생성 헤더는 원본을 `#include <철자>`로 포함하므로 각 `--include` 철자는 이 검색 순서에서 실제로 `--input` 파일로 풀려야 한다. 앞선 디렉터리의 다른 헤더가 같은 철자를 가리면 MetaCompiler가 RMC1000으로 거부한다. 빌드는 그 전에 public 리플렉션 헤더를 쓰는 타깃 안 모든 모듈의 검색 순서로 이를 확인하고, 생성 철자(`.generated.h`·모듈 생성 헤더)가 생성 루트보다 앞선 디렉터리의 파일에 가려지는지도 본다(RMC1001). 생성물 검증 컴파일은 검색 순서에서 출력 루트가 있던 자리에 검증용 임시 루트를 넣어 소비자와 같은 순서로 푼다.

반복 가능한 `--clang-arg`는 현재 네이티브 Debug/Release/ASan 프로필, 붙임형 `-DNAME=value` 및 clang-cl의 `/DNAME=value`, macOS에서 xcrun이 선택한 `-isysroot` 경로와 최소 버전만 허용한다. 경고 억제, 임의 검색 경로, 강제 include, response file과 그 밖의 미지원 인자는 거부한다. 옵션 확장은 네이티브 컴파일과 AST에 적용되는 조건이 같은지 검토하고 양성·음성 회귀를 추가한 후 수행한다.

C++ 구문은 원문 정규식이 아닌 실제 Clang JSON AST에서 읽는다. 선언의 annotation 인자, 접근 지정, 기반 관계와 컴파일러가 확정한 필드 타입을 사용한다. `--include`는 출력 헤더에 넣을 논리 경로이며 절대 입력 경로는 생성물에 포함하지 않는다.

생성 형식은 `ROCKS_CLANG_FORMAT` 환경변수를 우선 사용하고 clang-format 21.1.6을 요구한다. 같은 입력·타깃 문맥이면 같은 UTF-8/LF 바이트를 생성한다. 생성물은 출력 루트 옆의 고유 임시 디렉터리(`.rocks-meta-validation-*`)에 두고, 생성 헤더와 모듈 생성 헤더마다 따로 실제 Clang으로 검사한 뒤 게시한다(생성 소스는 `--verify-sources`를 준 경우에만 여기서 검사한다. 검사는 `--jobs` 수만큼 함께 돈다). 정상 종료와 관리된 실패에서 임시 디렉터리를 정리한다. 기존 출력과 같으면 다시 쓰지 않아 timestamp를 보존한다. 변경된 결과는 같은 디렉터리의 고유 임시 파일에 완성한 뒤 교체한다. 단독 실행은 입력에서 빠진 헤더의 이전 생성물을 지우지 않는다(지우기는 빌드의 게시 트랜잭션이 맡는다). `--check`는 모든 생성 헤더·모듈 생성 헤더·생성 소스·descriptor를 기존 출력과 비교하고 생성 루트(`Public`·`Private`·`Source`)에 남은 잉여 파일도 stale로 거부하며, 출력을 수정하지 않는다. 서로 다른 생성 설정이 같은 출력을 공유하는 빌드 그래프는 금지하며, 고유 임시 파일 사용이 그런 논리 충돌을 해결하는 것은 아니다.

C# 빌드 도구는 타깃에서 도달하는 모듈 가운데 리플렉션 헤더(아래 "모듈 메타데이터"의 표지 헤더)가 있는 모듈을 의존 순서로 하나씩 생성한다. 생성 디렉터리 `<out>/Generated/<M>/Public`·`Private`는 각각 그 모듈의 public·private include 디렉터리가 되어 의존 모듈로 전파된다. `<out>/Generated/<M>/Source`의 생성 소스는 include 루트가 아니고 그 모듈의 번역 단위로 컴파일된다. 생성은 `<out>/.metadata-stage/`에 하고, 의존 모듈은 stage의 descriptor와 생성 루트를 본다(이전 빌드의 게시본을 보지 않는다). 모든 모듈의 생성·`ids verify`·`ids verify-set`이 성공하면 생성 헤더·생성 소스·descriptor(`<out>/Metadata/<M>.rocksmeta.json`)와 입력에서 빠진 헤더의 이전 생성물 삭제를 한 트랜잭션으로 게시한다. 게시 중 실패하면 교체와 삭제를 모두 되돌리며, 실행 동안 명세가 바뀌었으면 게시하지 않는다. Core-only 구성은 이 생성 작업을 요구하지 않는다. 생성 헤더는 public include 헤더 검사에서 일반 엔진 헤더와 같은 경고 오류 기준으로 단독 컴파일된다.

## 진단과 수정

진단은 가능한 원본 파일과 줄, 오류 코드, 원인, `hint`를 함께 출력한다. 예를 들어 지원하지 않는 `PROPERTY` 지정자는 `RMC2000`으로 거부한다. `RMC1000`은 잘못된 명령·경로·지원하지 않는 컴파일 옵션, `RMC1001`은 리플렉션 헤더 철자를 가리는 다른 헤더, `RMC2001`은 처리할 수 없는 Clang AST 형태, `RMC2003`은 모듈 입력에도 import한 descriptor에도 없는 리플렉션 선언·base·참조 대상, `RMC2004`는 같은 모듈 또는 같은 타입을 두 번 import하거나, 자기 descriptor를 import하거나, import한 타입 이름을 이 모듈이 다시 정의한 경우, `RMC2005`는 대소문자만 다르거나 겹치는 생성 철자와 예약된 `.module.h`·`.generated.h` 이름, `RMC2006`은 private 타입을 public 타입의 base로 쓰거나 다른 모듈에서 쓴 경우(배열 원소 포함), `RMC2202`는 지정자를 그 필드 종류에 쓸 수 없는 경우(Required는 참조·참조 원소 배열·정적 배열·Set·그 값의 Map만, MaxElements는 배열·Map·Set만), `RMC2304`는 배열 원소로 쓸 수 없는 형식(Clang의 `Array` static_assert 실패를 멤버 위치로 바꾼 것 포함 — 위 "리플렉션 배열"), `RMC2305`는 Map·Set 오류(키·원소 종류, 키·값·원소의 컨테이너·Optional·cv·지원하지 않는 형식, 강한 참조 값·참조 struct 값, 기본이 아닌 해시·같음 정책, `HashMap`·`HashSet` PROPERTY — Clang의 `OrderedMap` static_assert 실패를 멤버 위치로 바꾼 것 포함, 리플렉션 B9f2), `RMC2307`은 정적 배열 오류(다차원, N 범위 1..65,535, 원소 형식 — 리플렉션 B9g), `RMC2210`은 설정 struct·설정 지정자 오류(Settings 묶음 값의 모양·예약 섹션, 설정 struct 밖의 설정 지정자, 콘솔 이름 문법·중복, 효력 시점 둘, 강한 참조를 담은 설정 struct — 설정 CF1), `RMC2100`은 명세 누락·변경과 inventory 불일치, `RMC2101`은 명세 사이 ID·이름 중복, `RMC2102`는 Metadata가 없는 모듈의 활성 명세, `RMC3000`은 Clang 입력 또는 생성 코드의 계약 오류, `RMC3100`은 포매터 오류, `RMC4000`은 `--check`의 stale 출력을 나타낸다. 실패 시 기존 생성물은 유지하며 빌드는 실패로 판정한다.

### 상속 export(RMC2002)

다른 모듈의 리플렉션 클래스를 상속하려면 그 기반과 그 위 조상(`rocks::Object` 아래까지)이 클래스 전체를 모듈 API 매크로로 export해야 한다(`class ROCKS_<기반 모듈>_API Base : public Object`). 파생 클래스가 기반의 생성자·소멸자·가상 함수를 링크하기 때문이다(Modular). 메타 컴파일러는 선언부의 매크로를 descriptor 클래스 flags `Exported`로 전하고, 없는 기반을 상속하면 `RMC2002`로 거부한다. 같은 모듈 안의 상속은 매크로가 필요 없다. 루트 `rocks::Object`는 이미 export되어 있다. 자세한 규칙은 [빌드 타깃](../Design/BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 BS8d3이다.

## 현재 한계와 검증 범위

리플렉션 B9(A0-O 다음, A0-G 앞)가 지정자(CLASS `Transient`·`Deprecated`·`Within`, 타입 `Name`, 필드 `DuplicateTransient`·`NonTryoutDuplicateTransient`·`SaveGame`·`TextExportTransient`)·descriptor 키(`key`·`element`·`within`·`base`·`toolName`·`bitfield`, 클래스 flags `Transient`·`Deprecated`)·텍스트 codec(`ReflectionText`)을 더한다. 구현하면 이 문서의 지정자 표·struct·descriptor 절을 단계별로 고친다([리플렉션 B9](../Design/ReflectionExtensions.ko.md) 1.8·16절). STRUCT `Atomic`·descriptor struct flags `Atomic`·Core 값 타입 미러 17종(Transform3f·Transform3d)은 B9b2에서, CLASS·STRUCT `Name`(타입 도구 이름)·descriptor 클래스·struct 항목의 `toolName`(실효 타입 도구 이름 — 읽기는 형식을 검사한다, RMC1000)·`ClassRef<Base>` 필드는 B9d2에서 반영했다.

모듈의 리플렉션 헤더는 한 번역 단위로 처리하고, 그 모듈 입력 헤더에 물리적으로 있는 선언만 생성 대상으로 모은다. include로 들어온 다른 모듈의 리플렉션 선언은 import한 descriptor에 같은 이름·같은 헤더 파일로 있어야 하며 다시 생성하지 않는다. compile_commands 기반 정확한 타깃 문맥 복원, 원격·로컬 생성 캐시, depfile 기반 최소 재생성은 아직 지원하지 않는다. 여러 타깃의 define·include 차이는 호출 측에서 정확히 전달해야 한다. 컴파일러나 타깃 문맥이 달라져도 항상 같은 산출물을 보장한다는 의미는 아니다.

런타임 회귀는 scalar/Guid/UTF-8 소유 값, 상속 관계와 native cast 분리, 잘못된 context·thread, staged write와 참조 barrier, typed target 거부, descriptor 함수표의 깊은 복사, String OOM·논리 파괴 재진입, Swap의 allocator 소유권, 등록 OOM 후 기반 의존성 원복을 검사한다. 계수·allocator ledger가 종료 후 0으로 돌아오는지도 확인한다.

C# 하네스는 실제 Clang을 호출하여 자동 ID, 잘못된 명세·중복·누락, 빠진 annotation, 미지원 필드·상속·Trace, 컴파일 오류와 경고 강등 옵션을 검사한다. 오류가 기존 출력 파일을 훼손하지 않는지, 같은 입력의 바이트·timestamp가 유지되는지, `--check`가 변경 없이 stale 출력과 잉여 생성물을 거부하는지도 검사한다. 모듈 모드는 여러 헤더·집계 헤더·descriptor 생성, 의존 모듈 import(상속 필드·참조 대상 ID), private 헤더 분리와 private 헤더만 있는 모듈의 의존 등록, 같은 이름·다른 파일의 선언, 자기 descriptor import·import 타입 재정의, descriptor의 엄격한 읽기, RMC2003·2004·2005·2006을 검사한다. identity 명령은 `ids move`의 ID·별칭 보존·중첩 타입·재실행, `ids verify-set`의 ID·이름 중복, ID 기준으로 합친 merge-check와 명세를 옮긴 Git 병합을 검사한다. 빌드 통합 시험은 MetadataLayout의 가림(RMC1001)·이중 소유·생성 철자 충돌·잘못된 선언·고아와 예약 명세·타깃 범위, 게시 트랜잭션의 삭제·되돌리기·백업 위치를 검사한다. 설치형 SDK 시험은 SDK descriptor import와, 메타데이터 모듈 두 개(라이브러리와 실행 파일)가 있는 게임에서 stage import, 상위 모듈 실패 시 미게시, Metadata에서 뺀 헤더의 생성물 삭제, 라이브러리 모듈에서 상속한 배열 필드(`Array<int32>`·`ObjectRefArray`·`Array<rocks::Vec3>`·`StructRefArray`)의 그래프 저장·로드와 배열 원소로만 닿는 객체의 수집 생존을 검사한다. 배열 메타데이터 시험(B5c)은 컨테이너·원소별 descriptor·생성 코드, RMC2304 변환(멤버 위치·clang과 clang-cl 위치 형식·NATIVE의 RMC2205), 원소·지정자·순환·깊이·형질 진단, 원소 struct 모듈 → base 모듈 → 파생 모듈의 세 모듈 구성, descriptor 조작(RMC1000·2003·2006), 생성 형질 static_assert probe, 컨테이너 명시적 인스턴스화 Policy를 검사하고, 그래프 시험(`GeneratedArrayGraphTests`)이 생성 코드로 등록·왕복·재저장 byte 동일·생성 Trace의 원소 방문을 검사한다. 생성 path thunk 시험(B6b)은 Struct·Array 필드와 모든 struct 멤버에만 thunk가 나오는지·대상 종류(원소·enum·Binds 실제 타입)·세 모듈 상속 import 필드의 소유자와 멤버 포인터·대상 종류를 바꾼 생성 헤더의 컴파일 실패(MetaTests·ModuleMetaTests), 생성 코드로 등록한 fixture의 모든 종류를 경로로 읽기·쓰기·삽입·삭제·비우기·도구 이름·ReadOnly 전파·경로 쓰기 뒤 그래프 왕복과 재저장 byte 동일(`GeneratedPathTests`), 설치 SDK 게임에서 다른 모듈에서 상속한 필드의 경로 쓰기·원소 삽입·삭제 뒤 저장·로드와 재저장 byte 동일을 검사한다. 런타임 시험은 공유 모듈의 한 번 등록과 lease 보유, Busy에서 역순 해제 중단과 재시도, 다른 native 타입의 같은 ID 거부, 성공 실행에서 잰 모든 할당 실패 지점의 되돌리기를 검사한다. 리플렉션 base를 둔 native 파생 시험(B8a — `NativeReflectedBaseTests`·`NativeDerivedGraphTests`)은 WorldOnly fixture(`WorldFixture`)와 두 단계 리플렉션 사슬(`SceneFixture : WorldFixture` — Transform 필드), WorldOnly가 아닌·Abstract 리플렉션 base 위의 .cpp native 파생으로 등록·WorldOnly만 상속·base 필드(추상 base의 private 필드 포함) 읽기·쓰기·경로 접근·struct 의존과 그 미등록 NotFound, 손 Trace(private·protected 포함)와 생성 Trace의 간선을 한 번씩 방문하는지와 수집 생존, 두 단계 native 사슬, base 해제 Busy, WorldOnly를 빠뜨린 수동 등록 거부, `ObjectTypeIdOf` 두 경로, 그래프 저장 Unsupported(루트·도달)와 로드 Unsupported(원인이 레이아웃 선언임을 같은 C++ 타입의 선언 등록과 대조)를 검사한다. World 타입 시험(B8b — EngineTests의 `WorldReflectionTests`·`WorldTests`·`EngineTests`)은 네 타입의 WorldOnly·필드 형태·멱등 등록, native 시험 파생의 base 상속, GC(생성 Trace·root 참조·Marking 중 AddComponent의 newborn shade·Marking 중 참조 쓰기의 삽입 장벽·root 파괴와 우회 파괴의 대표 비우기), Level 그래프 저장 → 별도 Registry 로드 → 재저장 byte 동일, native 파생이 든 Level 저장 Unsupported, 장부 밖 객체 거부, null 원소 순회, 경로 읽기·World 필드 쓰기 거부·fixture 쓰기, Engine 등록 순서와 native 파생을 둔 Shutdown·초기화 실패 정리를 검사하고, ModuleMetaTests `Meta.ModuleNameMayEqualAReflectedClassName`이 모듈 이름과 같은 이름의 rocks 클래스(World 모듈과 `rocks::World`)가 생성·검증 컴파일에서 서로 가리지 않음을 본다. MetaTests `Meta.ClassStaticTraceWrapsOpsTrace`가 클래스 static Trace 생성을, `Meta.NativeClassTraceDeclarationIsChecked`가 실제 컴파일러로 손 Trace 선언 검사(접근 지정별 허용, 다른 모양·사슬 기반의 거부)와 매크로 누락·native도 리플렉션도 아닌 기반의 안내 static_assert를, self `Policy.ReflectionAnnotationsRejectNativeTypeAndUnlisted`가 Policy 양성·음성 사례(목록 안 주석 항목, 중첩 클래스, 본문 주석 속 매크로, 리터럴이 아닌 Metadata 형태들)를 검사한다.

이전 구현은 Windows 로컬 Debug·Release·ASan과 [Windows/macOS native CI](https://github.com/maxidea1024/rocks/actions/runs/42775786556)에서 생성 코드 및 실제 Clang 회귀 실행을 확인하였다. 최종 수치·산출물 hash는 [5차 검증 기록](../Reviews/ImplementationRound5.FoundationValidation.ko.md)을 따른다. macOS는 ARM64 Debug·ASan 범위이며 Intel Mac과 TSan은 미검증이다.

## 모듈 메타데이터

리플렉션 헤더는 모듈 규칙에 적지 않는다. 빌드(`MetadataLayout` — `ReflectionHeaderScan`)가 모듈 폴더(`ModuleDirectory`) 안의 public·private include 디렉터리에서 `*.h`를 훑어, 주석·문자열을 지운 줄이 `ROCKS_CLASS(`·`ROCKS_STRUCT(`·`ROCKS_ENUM(`으로 시작하는 헤더를 리플렉션 헤더로 삼는다(UE UBT의 `UCLASS` 줄 첫머리 표지와 같다 — [설계](../Design/ReflectionFoundations.ko.md) 1.10절). 철자는 찾은 include 디렉터리 기준이고 목록은 철자 순서다. public 헤더의 타입은 의존 모듈이 쓸 수 있고, private include 디렉터리 헤더의 타입은 그 모듈 안에서만 쓴다. 모듈 폴더 밖 include 디렉터리(서드파티)와 모듈 시험(`Tests/`)은 훑지 않는다.

```csharp
public sealed class ReflectionTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ReflectionTestFixtures";

    protected override string[] PublicDependencies => ["ObjectRuntime"];

    protected override bool HasSources => false;
}
// Public/ReflectionTestFixtures/ReflectionFixture.h 등 표지가 있는 헤더를 빌드가 찾는다.
```

생성 헤더는 원본 철자의 `.h`를 `.generated.h`로 바꿔 include한다(`#include <ReflectionTestFixtures/ReflectionFixture.generated.h>`). 명세는 모듈 폴더의 `ReflectionTestFixtures.rocksids.json`이다. `ModuleRules`를 직접 상속하는 게임 모듈은 `NativeModuleInput`에 명세가 있는 폴더 `ModuleDirectory`(프로젝트 상대 경로)를 적는다(`Templates/Game/Foundation/FoundationTemplate.Build.cs`).

빌드는 다음을 거부한다: 모듈 규칙의 `Metadata` 목록(빌드가 찾으므로 적지 않는다), 리플렉션 헤더가 있는데 명세가 없는 모듈(RMC2100), 리플렉션 헤더가 없는데 활성 항목이 있는 명세(RMC2102 — 은퇴 항목만 남은 명세는 ID 예약이므로 허용), 모듈 소스 폴더 `Private/`(include 디렉터리가 아님)의 표지 헤더(RMC2003), 한 헤더를 두 모듈이 찾음, 대소문자만 다른 생성 철자·생성 헤더 접미사(RMC2005). 등록하지 않은 헤더의 `ROCKS_CLASS` 선언은 일반 C++ 컴파일만으로 누락이 탐지된다고 보장하지 않는다. 컴파일 단위와 생성 선언의 coverage를 강제하는 기능은 후속 구현 항목이다.