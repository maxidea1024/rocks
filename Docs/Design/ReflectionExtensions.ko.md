# 리플렉션 보강(리플렉션 B9) 설계

기준일: 2026-11-15, 확정안(설계 교차검증 3건 — UE 5.8 대조, Rocks 코드·스펙 정합과 구현 가능성, 소비자 적합성 — 과 사용자 결정(2026-11-15), 반영 확인 리뷰 1건과 조정자 결정을 반영). [에셋 시스템 설계](AssetSystem.ko.md)(이하 AS) 1절의 A0-O와 A0-G 사이에 두는 리플렉션 보강 단계다. 기준 코드는 `a4a7bcc`(A0-B B1–B8, A0-O Oa–Oe 구현 완료)다. 이름은 [엔진 구현 계획](EngineImplementationPlan.ko.md)(이하 EIP)의 벤치마크 ID `B9`(EIP 2416·2538행)와 구별하려고 문서에서 "리플렉션 B9"라고 쓴다. 하위 단계는 B9a–B9m이고 문자 순서가 의존 순서다.

근거: [리플렉션·빌드 보강 설계](ReflectionFoundations.ko.md)(이하 RF) 3–7절과 12절 열린 문제, [객체·그래프 보강 설계](ObjectFoundations.ko.md)(이하 OF) 1·4·5절, [런타임 클래스 메타데이터](RuntimeClassMetadata.ko.md)(이하 RCM) '클래스 기본값 결정', [Sparks 언어](SparksLanguage.ko.md)(이하 SL), [UI 툴킷](UiToolkit.ko.md)(이하 UI) CU4·CU8, [리플렉션 메타데이터 구현](../Implementation/ReflectionMetadata.ko.md)(이하 RM), 현재 코드(`Core/Source/Runtime/MetaSchema`·`ObjectRuntime`·`ObjectSerialization`·`Serialization`·`Core`·`Localization`, `Programs/Rocks.MetaCompiler`). UE 대조는 UE 5.8(5.8.4, commit `f45729c0b6f0`) 원본을 파일:행으로 확인했다(18절).

원칙(RF와 같다): 실패 원자성(읽기 실패는 출력 불변, 쓰기 실패는 미게시, 로드는 전부 공개 또는 전부 rollback), 결정성(같은 입력 → 같은 byte), 모든 길이·개수·작업량에 한도, 하위호환 없음(`FieldSchema`·wire tag·`PathStep`·생성 코드 형식을 바로 바꾸고 시험을 고친다). 필드 접근은 생성 thunk만 쓰고 raw offset을 공개하지 않는다(RF 원칙). 범용 기능(해시·텍스트 분해·숫자 표기·JSON)은 Core 공용 모듈에 두고 기능 모듈에 다시 만들지 않는다.

약어:

- Rocks 코드: `MS` = Core/Source/Runtime/MetaSchema/Public/Meta, `OR` = Core/Source/Runtime/ObjectRuntime/Public/Object, `ORP` = Core/Source/Runtime/ObjectRuntime/Private, `OS` = Core/Source/Runtime/ObjectSerialization, `MC` = Programs/Rocks.MetaCompiler, `CORE` = Core/Source/Runtime/Core/Public/Core.
- UE: `CUO/` = Engine/Source/Runtime/CoreUObject/, `UT` = CUO/Public/UObject/UnrealType.h, `OM` = CUO/Public/UObject/ObjectMacros.h, `CH` = CUO/Public/UObject/Class.h, `CC` = CUO/Private/UObject/Class.cpp, `PR` = CUO/Private/UObject/Property.cpp, `NET` = CUO/Public/UObject/NoExportTypes.h, `UOG` = CUO/Private/UObject/UObjectGlobals.cpp, `UHT/` = Engine/Source/Programs/Shared/EpicGames.UHT/, `PM` = UHT/Specifiers/UhtPropertyMemberSpecifiers.cs, `CS` = UHT/Specifiers/UhtClassSpecifiers.cs. 파일 이름만 쓴 UE 파일의 위치는 18.0절 표에 있다.

## 0. 범위와 현재 상태

| 항목 | 현재(`a4a7bcc`) | 리플렉션 B9 뒤 |
|---|---|---|
| 값 종류 | 스칼라 9종·Guid·String·AssetId·Digest256·ObjectRef·SoftObjectRef·Enum·Struct·Array(MS/TypeSchema.h:79-102) | + Int8·Int16·Name·WeakRef·ClassRef·Optional·Map·Set·StaticArray·InstancedStruct·Text, bitfield bool(Bool) |
| enum underlying | uint8·uint16·uint32·int32(RF 3.4절, RMC2401) | UE와 같은 8종 |
| struct | 상속 금지(RMC2301), 타입 플래그 없음 | 단일 리플렉션 base + 필드 없는 비리플렉션 base, `Atomic` |
| Core 값 타입 미러 | 15종(`CoreTypeBindings/CoreValueTypes.h`) | 17종(Transform3f·Transform3d 추가), 16종 Atomic |
| 필드 플래그 | Transient·ReadOnly·EditorOnly·Required | + DuplicateTransient·NonPIEDuplicateTransient·SaveGame·TextExportTransient |
| 클래스 | Abstract·WorldOnly | + Transient·Deprecated·Within, 타입 도구 이름 |
| 컨테이너 | Array·ObjectRefArray·StructRefArray | + Core `OrderedMap`·`OrderedSet`, `ObjectRefMap`, 정적 배열 |
| 경로(B6) | 필드 + 배열 index | + Map key·Set 원소 단계, 대입 묶음(Prepare → Publish) |
| 텍스트 표기 | 없음(RF 650행 "도구 계층") | 새 모듈 `ReflectionText`(명령행·JSON) |
| wire | `WireType` 1–13(Serialization/Record.h:10-28) | + Optional 14·Set 15·Map 16·InstancedStruct 17·Text 18 |

이 단계가 하지 않는 것: 함수 리플렉션(CU2)·delegate(CU3)·interface(CU9)·편집 메타데이터(CU1)·setter·통지(CU5)·동적 속성(CU6), Replicated 계열(네트워크 단계), Instanced 하위 객체(prefab), Config 계열(프로젝트 설정 계층), 클래스 기본값 레코드·차이 쓰기·override 집합(A5 — 이 문서는 단위 규칙만 정한다), 객체 단위 EditorOnly(A1), Sparks 바인딩 계열(`SoftClassRef`·`ScriptName`), custom Serialize·Identical, TLazyObjectPtr(AssetId Soft + SubobjectGuid가 대체), `TSubScriptStructOf` 대응(Sparks 데이터 테이블 단계), `ObjectRefSet`(객체 정체성을 키로 쓰는 집합), struct·실수 키, UE `NonTransient`(20절 결정 15).

## 1. 공통 기반

### 1.1 스키마

`ValueKind` 끝에 붙인다(MS/TypeSchema.h:79-102, 지금 마지막 Digest256 = 18).

| ValueKind | 값 | C++ 선언 | wire | 단계 |
|---|---|---|---|---|
| Int8 | 19 | `int8`(Clang 철자 `signed char`) | I64(4) | B9a |
| Int16 | 20 | `int16`(`short`) | I64 | B9a |
| Name | 21 | `rocks::Name` | String(8) | B9c |
| WeakObjectReference | 22 | `WeakRef<T>` | 참조 wire(graph v2 U32) | B9d |
| ClassReference | 23 | `ClassRef<Base>` | Guid(7) | B9d |
| Optional | 24 | `Optional<T>` | Optional(14) | B9e |
| Map | 25 | `OrderedMap<K,V>`·`ObjectRefMap<K,T>` | Map(16) | B9f |
| Set | 26 | `OrderedSet<T>` | Set(15) | B9f |
| StaticArray | 27 | `T x[N]` | ValueArray(13)·RecordList(11) | B9g |
| InstancedStruct | 28 | `InstancedStruct<Base>` | InstancedStruct(17) | B9j |
| Text | 29 | `rocks::Text` | Text(18) | B9l |

bitfield bool은 새 종류가 아니다(`Bool`, wire Bool(1) — 14절).

`FieldSchema`(MS/TypeSchema.h:115-140):

- 끝에 `ValueType key{}`를 붙인다. Map만 쓴다(그 밖은 Opaque).
- `element`의 뜻을 넓힌다: Array·StaticArray·Set의 원소, Map의 값, Optional의 내부 형식.
- `maxElements`: Array·Map·Set은 길이 상한(기본 65,536, 최대 1,048,576 — 지금과 같다). StaticArray는 **정확한 원소 수 N**이다.
- `value.type`: 지금의 Enum·Struct·ObjectReference·SoftObjectReference에 더해 WeakObjectReference(기대 대상, 0 = Object)·ClassReference(Base 클래스 ID, 필수)·InstancedStruct(Base struct ID, 0 = 제한 없음).
- `FieldTargetType`(MS/TypeSchema.h:149-151)은 컨테이너 종류(Array·StaticArray·Set·Map·Optional)면 `element`를 돌려준다. 새 `FieldKeyType`이 Map 키를 돌려준다.
- 등록 규칙(RF 3.5절) 개정: `kind == ObjectReference` ⇔ value가 ObjectReference이거나, Array·StaticArray의 원소가 ObjectReference이거나, Map의 값이 ObjectReference(ObjectRefMap)다. WeakObjectReference는 `FieldKind::Value`다.

`TypeSchema`(MS/TypeSchema.h:202-210):

- `TypeId withinType{}`(클래스만, 0 = 제한 없음 — 9절)와 `Span<const char> toolName{}`(타입 도구 이름 — 5.2절)을 더한다.
- struct의 `baseType`을 연다(12절). RegisterStruct의 `flags != 0` 거부(ORP/TypeRegistryRegister.cpp:715-716)는 struct 전용 허용 mask(`KnownStructFlags` = Atomic)로 바꾼다.

`FieldValue`(OR/RuntimeTypeOps.h:60-91)에 칸을 더한다: `Name name`(B9c), `bool present`(B9e), `Text text`(B9l). ClassRef는 `guid` 칸(클래스 TypeId), WeakRef는 `reference` 칸을 쓴다. Optional 단일 값의 `kind`는 **내부 종류**이고 `present`가 set 여부다.

Map 키 대상: 지금 등록·lease·codec frame은 필드당 대상 하나다(TypeRegistryRegister.cpp:657-704 `targets[index]`, RuntimeTypeOps.h `valueNativeType`, `TypeLease::FieldEnum`·`FieldStruct`, `FieldCodecFrame::enumeration`). Map 키가 Enum·ClassRef·Soft면 키 대상이 따로 필요하므로 `RuntimeFieldOps::keyNativeType`·`RuntimeStructFieldOps::keyNativeType`, 등록 레코드의 키 대상 포인터(의존 계수 포함), `TypeLease::FieldKeyEnum(index)`, `FieldCodecFrame::keyEnumeration`을 더한다(B9f2).

### 1.2 wire

`WireType`(Serialization/Record.h:10-28)에 더한다. 정수는 little-endian, Guid·String·Bytes는 Archive 규칙이다.

| WireType | 값 | payload |
|---|---|---|
| Optional | 14 | u8 present(0·1, 그 밖은 InvalidArgument). 1이면 u32 내부 wire + 내부 payload. 내부가 struct면 Record 바이트(남은 길이 전부) |
| Set | 15 | u32 원소 wire, u32 count, 원소 payload × count |
| Map | 16 | u32 키 wire, u32 값 wire, u32 count, (키 payload, 값 payload) × count. 값이 Record면 u64 길이 + Record. 값이 InstancedStruct면 u64 길이 + (0이면 비어 있음, 아니면 Record) |
| InstancedStruct | 17 | 필드 값으로는 빈 payload(0 byte) = 비어 있음, 아니면 Record(schema = 실제 struct ID, version = 저장 당시 struct 버전). ValueArray 원소·Map 값 자리에서는 u64 길이 + 같은 내용 |
| Text | 18 | u8 형태. 0 = 비어 있음, 1 = Invariant(String), 2 = Localized(Guid textId, U64 sourceRevision, String fallback). 그 밖은 InvalidArgument |

- ValueArray·Set·Map 원소·키·값으로 받는 tag는 Bool·U32·U64·I64·F32·F64·Guid·String·Bytes·SoftObjectReference(지금, RF 5.4절)와 InstancedStruct(17)·Text(18)다. Record·RecordList·ValueArray·Optional·Set·Map은 원소·키 tag가 아니다. struct 원소 배열은 지금처럼 RecordList다.
- reader는 tag가 schema 종류의 wire와 같은지 보고, count를 할당 전에 schema 종류의 최소 크기(InstancedStruct·Record 값 8, Text 1, 그 밖은 RF 5.4절)로 남은 byte와 비교하며, count만큼 work를 먼저 청구한다.
- Serialization reader는 형식(tag·길이·count·잘림)만 본다. Set 원소·Map 키의 중복은 codec 1단계가 종류 규칙으로 판정한다(String은 byte, Name은 Core Name 규칙 — 7.2절). 중복은 Set·Map 모두 InvalidArgument다.
- Set·Map을 ValueArray로 쓰지 않는 이유: Record만 읽어도 종류가 드러나고, Array → Set 같은 schema 변경이 조용히 통과하지 않는다.
- 그래프 한도(OF 5.6절): Set 원소·Map 항목·StaticArray 원소는 `maxTotalElements`에 1씩, Map 값·Optional 내부·InstancedStruct의 Record 필드는 `maxTotalFields`에 센다.

### 1.3 종류 성질 표

값 종류의 성질을 MetaSchema(`MS/TypeSchema.h`)의 constexpr 함수 하나로 모은다. 지금은 원소 종류 목록이 OR/GeneratedFieldAccess.h·OR/GeneratedFieldCodec.h·ORP/TypeRegistryRegister.cpp·MC/SchemaValidator.cs·MC/SchemaModel.cs·MC/ModuleDescriptor.cs(374-442행)에 반복되고, TypeRegistry는 `<= Digest256` 숫자 범위로 지원 종류를 판정한다(TypeRegistryRegister.cpp:73-80). 이 판정은 표 조회로 바꾼다. wire 대응은 MetaSchema가 Serialization에 의존하지 않도록 ObjectRuntime codec의 switch 하나(`FieldWireType` — GeneratedFieldCodec.h:17-54)에 둔다.

성질(값 종류마다 bool): 단일 필드, struct 멤버, Array 원소, StaticArray 원소, Map 키, Map 값, Set 원소, Optional 내부, InstancedStruct 내용, Required 허용, 대상 타입 있음, 대상 ID 필수, 컨테이너, FieldValue 통째 읽기·쓰기(비aggregate).

MetaCompiler(C#)와 런타임(C++)의 표 대조: MetaCompiler가 "모든 종류 × 모든 성질"의 C# 표를 `static_assert(rocks::ValueKindCanBeMapKey(rocks::ValueKind::Name) == true)` 꼴로 한 번에 내는 대조 헤더를 생성하고, ToolTests가 이를 Clang으로 컴파일한다. 양방향(C#이 허용하고 C++가 거부하는 칸, 그 반대)을 모두 잡고, 생성 헤더마다 비용이 들지 않는다. Sparks 컴파일러·LSP도 같은 constexpr 표를 쓴다(SL 311·1115행의 키·저장 가능 판정).

허용 표(○ 허용, × 거부, 이름은 쓰는 컨테이너):

| 종류 | 단일·멤버 | Array 원소 | StaticArray 원소 | Map 키 | Map 값 | Set 원소 | Optional 내부 | 통째 FieldValue |
|---|---|---|---|---|---|---|---|---|
| Bool·정수 8종 | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| Float32·Float64 | ○ | ○ | ○ | × | ○ | × | ○ | ○ |
| Enum(Flags 포함) | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| Guid·AssetId·Digest256 | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| String·Name | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| ObjectRef | ○ | ObjectRefArray | ○ | × | ObjectRefMap | × | × | ○ |
| WeakRef | ○ | ○ | ○ | × | ○ | × | × | ○ |
| SoftObjectRef | ○ | ○ | ○ | ○ | ○ | ○ | × | ○ |
| ClassRef | ○ | ○ | ○ | ○ | ○ | ○ | × | ○ |
| 값 struct(강한 참조 없음) | ○ | ○ | ○ | × | ○ | × | ○ | × |
| 참조 struct | ○ | StructRefArray | ○ | × | × | × | × | × |
| Array 계열·Map·Set·StaticArray·Optional | ○ | × | × | × | × | × | × | × |
| InstancedStruct | ○ | ○ | ○ | × | ○ | × | × | × |
| Text | ○ | ○ | ○ | × | ○ | × | × | ○ |

- "통째 FieldValue"가 ×인 종류는 aggregate다(read·write·writeStaged가 null, 경로 연산으로만 쓴다 — RF 3.6절 규칙을 넓힌다). Optional은 내부가 ○면 비aggregate, 값 struct면 aggregate다.
- 직접 중첩만 막는다. 값 struct가 Array·Map 멤버를 담고 그 struct가 원소가 되는 간접 중첩은 지금처럼 허용한다(UE도 직접 중첩만 막는다 — 18.7절).
- Optional 내부에서 참조를 막는 이유: 참조는 이미 null이 "없음"이다(SL 24행 "없음은 하나다", SL 1198행 SD19).
- InstancedStruct 내용으로 받는 struct는 11절 조건(강한 참조·Registry 컨테이너·InstancedStruct·Required 없음)을 지켜야 한다.

### 1.4 플래그 비트

| 플래그 | 값 | 대상 | 전파·상속 | 단계 |
|---|---|---|---|---|
| FieldFlags::DuplicateTransient | 16 | 필드 | 전파 없음 | B9h |
| FieldFlags::NonPIEDuplicateTransient | 32 | 필드 | 전파 없음 | B9h |
| FieldFlags::SaveGame | 64 | 필드 | 전파 없음(멤버마다 표시) | B9h |
| FieldFlags::TextExportTransient | 128 | 필드 | 전파 없음 | B9h |
| (예약) NonTransactional | 256 | 필드 | — | CU4가 의미를 정할 때 |
| (예약) AssetRegistrySearchable | 512 | 필드 | — | A1이 플래그 방식을 고를 때 |
| TypeFlags::Transient | 4 | 클래스 | 상속 | B9h |
| TypeFlags::Deprecated | 8 | 클래스 | 상속 | B9h |
| TypeFlags::Atomic | 16 | struct | struct base에서 상속 | B9b |

- 새 필드 플래그는 `PropagateFieldFlags`(MS/TypeSchema.h:52-57)의 전파 집합에 넣지 않는다. UE 전파 집합(OM:509-513)에도 없다. 바깥 필드가 제외되면 안쪽은 방문되지 않으므로 제외 플래그에는 전파가 필요 없고, SaveGame은 UE처럼 멤버마다 표시한다(9.1절).
- 예약 비트는 등록하지 않는다(알 수 없는 비트는 지금처럼 등록 거부 — TypeRegistryRegister.cpp:35-40). 문서에 번호만 잡는다.

### 1.5 경로 key 단계

`PathStep{Guid fieldId; uint32 index; bool indexed}`(MS/TypeSchema.h:155-159)를 바꾼다(B9f3).

```cpp
enum class PathSelector : uint8 { None, Index, Key };
// Map 키·Set 원소의 값이다. wire는 그 키 종류의 wire payload 정규형(ValueArray 원소와 같은 byte)이다.
struct PathKey {
    ValueKind kind = ValueKind::Opaque;
    Span<const uint8> wire{};   // 비소유
};
struct PathStep { Guid fieldId; uint32 index = 0; PathSelector selector = PathSelector::None; };
struct PropertyPath { Span<const PathStep> steps; Span<const PathKey> keys{}; };
```

- `selector == Key`면 `index`는 `keys`의 위치다. `IsCanonicalPathStep`: None이면 index 0, Key면 index < keys 수.
- `SamePropertyPath`는 key를 wire byte로 비교한다. 예외는 Name 키다: Core Name 규칙(ASCII 대소문자 무시·번호 접미)으로 비교한다(4절).
- key가 wire payload라서 경로를 Record로 그대로 인코딩할 수 있다. CU4의 소유형 경로(`OwnedPropertyPath`)와 그 Record 형식은 CU4가 정하고, 이 단계는 key 단계가 인코딩 가능함을 보장한다.
- 문자열 문법(대소문자 무시, 도구 이름): `name ('.' name | '[' index ']' | '[' key ']')*`. key는 schema의 키 종류로 해석하므로 따옴표 없이 쓴다(`Downscale.perPlatform[Android]`). `]`·`,`·`=`·`"`·공백·앞뒤 공백이 있을 때만 큰따옴표로 감싸고, 따옴표 안에서는 `\"`만 escape다. 그 밖의 `\`는 글자 그대로다(AS 13절 escape 규칙과 같은 원칙 — 10.4절 escape 표).
- `MaxPropertyPathSteps`(17)는 그대로다. key·index는 단계를 늘리지 않는다.

### 1.6 진단 경로

`FieldDiagnostic::path`·`SoftReferenceInfo::path`·`ExternalReferenceInfo::path`(OR/FieldCodecHooks.h)는 `Span<const PathStep>`이다. 진단 경로에서는 Map 항목·Set 원소·StaticArray 원소를 **삽입 순번 index**(`selector = Index`)로 표기한다. 1단계(worker일 수 있음)에서 키를 복사하지 않고 할당도 없다. key 단계는 편집 경로(B6·텍스트) 전용이다. `ElementIndexScope`(지금 Array가 아니면 Fatal)와 `FieldCodecFrame`을 StaticArray·Set·Map에 일반화한다(B9f2·B9g).

### 1.7 공통 규칙

- **누락 = 기준**(OF 4절): 새 종류도 같다. Record에 없는 필드·멤버는 기준 값이다. Optional의 unset, Map·Set의 빈 값, InstancedStruct의 비어 있음은 "있는 값"이며 누락과 다르다.
- **제외·포함 정책**: `FieldEncodePolicy`·`FieldDecodePolicy`(OR/FieldCodec.h:44-64)에 `includeFlags`를 더한다(9.1절). 판정 위치는 지금 excludedFlags와 같다(OF 4절 "구현에서 정한 점"). 새 컨테이너의 원소·키·값은 그 필드의 유효 플래그를 그대로 받는다.
- **두 단계 decode**(RF 6.2절): 1단계는 파싱·형식·한도·중복 키·TypeRegistry 조회만 하고 ObjectRegistry를 건드리지 않는다. 2단계는 할당 말고는 실패하지 않는다. Name intern만 그 사이 2a에서 한다(4절).
- **키 위치의 decode 정책**: 값을 바꾸는 정책(미선언 enum `ReportAndDefault`, `unknownClass = ReportAndNull`)은 Map 키·Set 원소에 적용하지 않는다. 키 위치는 늘 거부다. 대체 값이 서로 다른 두 키를 같게 만들어 2단계에서 중복이 생기는 일을 막는다.
- **allocator**: Optional·InstancedStruct·Text처럼 값이 없던 자리에 새 값을 만들 때는 객체 생성자에 주입되는 registry allocator(RuntimeTypeOps.h:400-420)를 쓴다. 공개 조회 `ObjectRegistry::ObjectAllocator()`·`ConstructionContext::ObjectAllocator()`를 더한다(B9e). 수명은 Registry·트랜잭션과 같고, 소유 스레드 계약은 다른 Registry API와 같다.
- **같음**(RCM 63행): 기준과 같은 wire byte 비교다. Map·Set은 삽입 순서까지, Name은 표시 문자열 byte로 비교한다.
- **저장 불가 객체**: 클래스가 Transient·Deprecated인 객체와 그 부분 트리(Outer 사슬에 그런 객체가 있는 객체)는 패키지 저장(저장 정책 Exclude)에서 빼고, 스냅샷·undo·PIE 복제(Keep)에서는 보존한다(9.2절). 판정 도우미 `IsUnsaveableObject`를 ObjectRuntime에 두고 A1 export 집합 구성과 그래프 저장이 같이 쓴다.

### 1.8 descriptor(`<M>.rocksmeta.json`) 형식 변경

모듈 사이 계약(RF 1.4절, MC/ModuleDescriptor.cs:13·19·374-442)을 바꾼다. 읽기는 지금처럼 모르는 속성을 거부한다(schema 번호는 올리지 않는다 — 하위호환 없음).

- 필드: `key`(Map), `element`(Optional·Set·StaticArray·Map 값), StaticArray의 `maxElements` = N, `bitfield`(bool, 14절).
- 클래스: `within`(정규 이름), `toolName`, flags에 `Transient`·`Deprecated`.
- struct: `base`(정규 이름), `toolName`, flags에 `Atomic`, 비리플렉션 빈 base는 기록하지 않는다(12절).
- 종류 검증(`IsSupportedValueKind`·`IsSupportedElementKind`·`KindAcceptsRequired`·`KindHasTargetType`)은 1.3절 C# 표를 쓴다.

## 2. 정수 폭(B9a)

- B9a1: 1.3절 성질 표와 대조 헤더. 동작 불변 리팩터다.
- B9a2: `int8`·`int16` 필드·멤버·원소. enum underlying을 UE와 같은 8종(`uint8`·`uint16`·`uint32`·`uint64`·`int8`·`int16`·`int32`·`int64`)으로 넓힌다.
- B9a3: Serialization의 wire 14–18 reader·writer와 ValueArray 원소 tag 확장(1.2절). 형식만 검증하는 시험을 먼저 고정한다.
- wire는 I64다. 정수 폭을 넓히는 schema 변경이 wire 호환으로 남는다(RF 결정 14). decode는 I64 값이 int8·int16 범위인지 검사한다(UInt8·UInt16의 `UnsignedLimit` — GeneratedFieldCodec.h:76-80과 같은 자리에 `SignedLimit`).
- 평범한 `char`는 거부한다(부호가 구현 정의). Clang desugar 철자 `signed char`·`short`를 MC/SchemaValidator.cs:5-20 표에 더한다.
- uint64 enum의 열거자 값은 int64 최댓값 이하여야 한다(`EnumeratorSchema::value`가 int64 — MS/TypeSchema.h:238-248, 위반은 RMC2401). Flags uint64 enum은 비트 0–62를 쓴다. 검증기 `IsDeclared(int64)`와 wire I64를 그대로 쓰기 위해서다.
- 경로·FieldValue는 `signedInteger` 칸, 텍스트는 10진이다.

## 3. Atomic·Transform3f·Transform3d(B9b)

### 3.1 Atomic

- `ROCKS_STRUCT(Atomic)` → `TypeFlags::Atomic`(16, struct 전용). descriptor flags `Atomic`, `StructView::IsAtomic()`. RegisterStruct는 struct 전용 허용 mask(Atomic)만 받는다. TypeRegistry.h의 "struct에는 클래스 플래그 없음" 주석을 고친다.
- **뜻: Atomic struct 값은 통째 단위다.**
  - 차이 저장(A5): 멤버 하나라도 기준과 다르면 struct Record를 쓰고 모든 멤버를 넣는다. 같으면 쓰지 않는다. 비교 대상은 struct Record 전체의 wire byte다.
  - override 집합(RCM 62행)의 단위는 그 struct 필드 경로다. 경로 쓰기로 멤버 하나를 바꿔도 struct 경로가 기록된다(10.2절의 override 경로 정규화).
  - 되돌리기 단위도 struct 통째다. 구현은 RCM 70행의 "struct 통째 되돌리기" 후속과 함께 하고, 이 단계는 단위만 정한다.
  - 텍스트 출력은 모든 멤버를 쓴다(기준 차이 출력이 생겨도).
  - SaveGame 포함 필터는 Atomic struct 안의 멤버에 적용하지 않는다(통째 — 9.1절).
  - 중첩: Atomic 안의 비Atomic 멤버도 통째로 쓴다. 비Atomic struct 안의 Atomic 멤버는 그 멤버 경로에서 멈춘다.
- 로드는 바뀌지 않는다. 누락 = 기준은 형식 규칙이다(writer가 늘 모든 멤버를 쓸 뿐이다). 경로 접근도 멤버 단위 그대로다.
- Atomic은 struct base에서 상속되고 파생이 끌 수 없다(12절, UE `STRUCT_Inherit`도 같다 — 18.2절).
- **UE `Atomic`과의 관계**: Rocks `Atomic`은 UE `STRUCT_Atomic`과 이름은 같지만 뜻은 UE `immutable` 수학 타입·native Serializer 타입의 결과(통째)를 따른다. UE `Atomic`만 켠 struct는 태그 저장에서 기준과 같은 멤버를 건너뛰고, 다른 멤버만 기준 없이 깊게 쓴다(CC:2016-2029). 이 "깊은 전체 쓰기"에 대응하는 Rocks 의미는 없다. UE 데이터를 옮길 때 `USTRUCT(Atomic)`은 Rocks `Atomic`보다 덜 쓴다는 점을 반입 도구가 알아야 한다.
- 이 단계는 플래그·MetaCompiler·descriptor·미러 표시·조회 API를 구현한다. 차이 쓰기 자체는 A5다.

### 3.2 Core 값 타입과 미러

Core 수학(`CORE/Math/Transform.h`)에 둘을 더한다(사용자 결정 U4).

| 타입 | 멤버(이 순서) | 계산 정밀도 |
|---|---|---|
| `Transform`(지금) | `Vec3d translation`, `Quat rotation`, `Vec3 scale` | double(EIP 2.3절 authoring LocalTransform) |
| `Transform3f`(새) | `Vec3 translation`, `Quat rotation`, `Vec3 scale` | float |
| `Transform3d`(새) | `Vec3d translation`, `Quatd rotation`, `Vec3d scale` | double |

- API는 `Transform`과 같은 수준이다: `Identity`, `IsValid`, `HasUniformScale`, `TransformPoint`·`TransformVector`·`TransformDirection`, `TryInverseTransformPoint`·`TryInverseTransformVector`, `ToMatrix`(Transform3f는 `Mat4`(float), Transform3d는 `Mat4d` — CORE/Math/Matrix4.h:316-317), `TryFromMatrix`, `TryCompose`, `TryInverse`. 점 변환 순서(scale → rotation → translation), 합성 규약(world = parent * local), shear면 근사하지 않고 Unsupported인 실패 규약은 `Transform`과 같다(EIP 2.3절).
- 변환: `Transform` ↔ `Transform3d`, `Transform` ↔ `Transform3f`, `Transform3f` ↔ `Transform3d`를 이름 있는 명시 함수로 둔다. 넓힘은 정확하고, 좁힘은 가장 가까운 값으로 반올림한다. 좁힌 결과가 유한한지는 호출자가 `IsValid`로 본다.
- EIP 2.3절과의 관계: authoring LocalTransform은 지금처럼 `Transform`(혼합 정밀도)이다. Transform3f·Transform3d는 정밀도가 하나인 값 타입이다(UE FTransform3f·FTransform3d 데이터 반입, GPU·애니메이션용 float 변환, 도구용 double 변환). EIP 2.3절과 `Transform.h` 머리 주석에 이 관계를 적는다(21절).
- 미러(`CoreTypeBindings/CoreValueTypes.h`): `Transform3fBinding`·`Transform3dBinding`을 더한다. 17종 중 **16종에 `Atomic`을 명시한다**: Vec2·Vec2d·Vec3·Vec3d·Vec4·Vec4d·Quat·Quatd·Rotator·Rotatord·Color·LinearColor·IntVec2·IntVec3·Transform3f·Transform3d. `TransformBinding`은 **비Atomic**이다(UE `FTransform`처럼 멤버 단위 — 멤버 Vec3d·Quat·Vec3는 각각 Atomic이라 멤버마다 통째). `TransformBinding`의 세 멤버에는 `SaveGame`을 단다(UE FTransform 멤버도 SaveGame이다 — NET:2120·2124·2128).
- prefab·instance override에서 이동만 덮고 회전·크기는 기준 변경을 따라가는 UE 동작이 `Transform`에서 그대로 유지된다.

### 3.3 RCM·RF 정정 문구

- RCM 62행 끝: "Atomic struct(리플렉션 B9b)는 그 struct 필드 경로가 단위이며 멤버로 내려가지 않는다."
- RCM 64행 첫 문장: "struct 필드는 기본적으로 **멤버 단위**로 비교·저장한다(UE 태그 직렬화의 멤버별 비교와 같다). 단 `Atomic` struct는 **통째 단위**다: 멤버 하나라도 기준과 다르면 모든 멤버를 쓰고, override 단위·되돌리기도 그 struct 경로다. Core 값 타입 미러 17종 중 `Transform`(혼합 정밀도)만 비Atomic이고 나머지는 Atomic이다. UE는 수학 타입을 native Serializer나 Immutable 이진 직렬화로 통째 쓰고, `FTransform`만 멤버 단위로 쓴다(리플렉션 B9 18.2절)."
- RCM 70행: "Atomic struct의 되돌리기는 통째다(구현은 struct 통째 되돌리기 후속과 함께)."
- RF 결정 22(1024행) 끝: "struct의 차이 단위는 Atomic 여부로 정한다(리플렉션 B9b)."

## 4. Name(B9c)

- C++ `rocks::Name`(세션 토큰 — CORE/Text/Name.h:10-28). 클래스 필드·struct 멤버·Array·StaticArray 원소·Map 키·값·Set 원소·Optional 내부.
- wire String(8). 값은 **표시 문자열**이다(번호 접미 포함, 예 `Socket_7`). None은 빈 문자열로 쓴다(표시 변형 `NONE`도 빈 문자열 — 결정적).
- 내용 규칙: 유효 UTF-8, NFC, NUL 없음, `MaxNameFieldBytes` = 3,072 byte 이하. UE `NAME_SIZE`는 null 포함 1,024 UTF-16 코드 단위라 이름 최대 1,023 단위이고(NameTypes.h:57, UnrealNames.cpp:3271), UTF-8로 최대 3,069 byte다. 3,072 byte면 UE 이름이 모두 들어간다. 쓰기·encode·decode 1단계가 같은 함수로 검사한다. NFC 판정은 Core Unicode를 쓰고 정규화는 하지 않는다(입력 계층인 ReflectionText가 NFC로 만든다 — OF 1.1절과 같은 분담).
- 비교: Core Name 규칙(ASCII만 대소문자 무시, 비ASCII는 byte 비교, UE 5.8 번호 접미 — CORE/Text/NamePool.h:35-39). UE FName도 정확히 ASCII만 접는다(18.3절). Map 키·Set 원소의 중복 판정도 이 규칙이다.
- 차이 저장의 같음은 표시 문자열 byte다. 대소문자만 바꾼 값은 "다름"으로 저장된다(의도한 편집이며 무해하다 — 사용자 결정 U7). UE는 `Identical`이 대소문자 무시라 저장하지 않는다.
- NamePool 위치: `RegistryOptions`에 `NamePool* names`를 더한다(빌림, Registry보다 오래 산다). Engine이 pool 하나를 만들어 에셋 Registry·World Registry에 같이 준다(전역 singleton 없음 — RCM 32행). Name 필드를 가진 타입을 pool 없는 Registry에 만들면 그 필드의 쓰기·복원은 Unsupported다.
- 생성자 기본값: 생성자는 `Allocator&`만 받으므로 Name 필드의 C++ 기본값은 None뿐이다. 기본 이름이 필요한 값은 프로파일·클래스 기본값 레코드(A5)·도구가 채운다.
- Core 추가(B9c1):
  - `NamePool::TryFind(StringView) -> Result<Name>`: intern하지 않는 조회, 없으면 NotFound. 표시 변형이 다른 입력에는 비교 identity 토큰(첫 표시)을 돌려준다.
  - pool 없는 비교 `NameTextEquals(StringView, StringView)`와 base·번호 분해 공개. 1단계 Map<Name> 중복 판정과 `SamePropertyPath`가 쓴다. 해시는 지금 `TryMakeNameKey`(Name.h:144)를 쓴다.
- 쓰기 thunk는 토큰의 domain이 그 Registry pool인지 확인한다(다르면 WrongContext).
- GC 없음. Required 거부(RMC2202).
- 텍스트: 출력은 그대로, None은 `None`(UE 비Delimited 출력과 같다). 입력은 빈 값·`None`(대소문자 무시)을 None으로 받는다.

### 4.1 복원의 intern

- 1단계는 Name 문자열을 검사하고 위치를 산출물에 차례로 기록한다. 2a(`CreateObjects` 사전 확인, Game 스레드)가 대상 Registry의 pool로 모두 intern해 노드별 토큰 표를 만든다. 2단계는 같은 순서로 토큰을 소비한다(마이그레이션 보관본을 차례로 대조하는 지금 방식과 같다 — GraphApply.cpp:101-113). 순서가 어긋나면 InvalidArgument(계약 위반)다.
- 작업량: 1단계가 Name 수 × (byte + 상수)를 2a 몫으로 미리 청구한다(OF 5.4절 Od2 "사전 확인 비용은 1단계가 청구한 몫이 덮는다"와 같은 방식).
- 실패: pool의 CapacityExceeded·OutOfMemory는 2a 실패로 로드를 끝낸다(생성자 실행 전). 손상이 아니므로 A1이 CorruptPackage로 바꾸지 않는다. 이미 intern한 entry는 남는다. pool entry는 pool 수명 자원이고 다시 쓰이므로 누수가 아니다(UE name table도 지우지 않는다). OF 5.7절 2a 실패 목록에 "Name pool 한도"를 더한다.
- 한 트랜잭션의 여러 Stage(SCC)는 각자 자기 2a에서 intern한다.
- 관찰 struct(`FieldCodecHooks::ObservedStructs` — 1단계 임시 값으로 넘긴다)는 Name을 (직간접으로) 담을 수 없다. 참조를 담는 struct와 같은 자리에서 InvalidArgument다(OR/FieldCodecHooks.h 관찰 규칙).

## 5. WeakRef·ClassRef(B9d)

### 5.1 WeakRef PROPERTY

- C++ `WeakRef<T>`(OR/ObjectRefs.h:196-214, handle만 담는다). 클래스 필드·struct 멤버·Array·StaticArray 원소·Map 값. `FieldKind::Value`, `value.type` = 기대 대상(0 = Object).
- GC: 방문하지 않는다(지금도 생성 Trace가 WeakRef를 방문하지 않는다 — RuntimeTypeOps.h:322-323). ReferenceObserver도 관찰하지 않고, 편의 복원의 도달 검사에도 넣지 않는다(OF 5.5절). 쓰기 장벽 없음.
- 쓰기 검증: 대상이 같은 Registry에서 살아 있고 기대 타입의 파생이어야 한다(`TypeRegistry::IsA`). null 허용.
- encode: 대상이 죽었거나 stale이면 null을 쓴다(진단 없음 — weak의 정상 상태).
- decode: 노드 번호면 그 객체 handle, 기대 타입 위반은 InvalidArgument(강한 참조와 같은 규칙. UE는 경고 후 null — 18.4절). 외부 번호(`0x80000000 | i`)는 writer가 쓰지 않으므로 InvalidArgument다.
- 같음(RCM 63행): non-null은 대상 정체성으로 비교한다.
- Required 거부(RMC2202). 런타임에 저절로 null이 되므로 "늘 non-null" 계약을 지킬 수 없다(UE weak 속성도 NonNullable이 될 수 없다 — PropertyWeakObjectPtr.cpp:56).
- 참조 wire는 문맥 매개변수(RF 6.1절)이므로 문맥에 `WriteWeakReference`·`ReadWeakReference`를 더한다(graph v2 = U32). `ExternalReferenceInfo`(OF 5.3절)에 `bool weak`를 더한다.
- 저장 단위: weak는 **같은 저장 단위(그래프) 안만** 보존한다. 셀 사이 entity 참조(SL 495행)는 World 단계의 영속 entity 참조가 맡는다.

저장 정책별 집합 밖 weak 처리(`GraphExternalPolicy` — Oe의 Reject·Admit·Include):

| 정책 | 집합 안 대상 | 집합 밖 대상 |
|---|---|---|
| Reject(기본) | localId | null + `ExternalReference` 진단(값 2 = weak null). 저장은 실패하지 않는다(강한 참조만 거부 대상) |
| Admit | localId | hook에 `weak = true`로 묻는다. weak는 같은 대상의 slot 캐시(강한 참조가 먼저 Import한 대상 포함)를 쓰지 않고 출현마다 묻는다. Null·Import면 null + 진단(값 2 — weak import는 없다), Reject면 진단(값 0) 뒤 InvalidArgument |
| Include(편의 저장) | 최종 집합 기준 localId | 출력 위치와 대상 handle을 기록해 두고, 모든 노드를 encode한 뒤 최종 집합에 있으면 localId, 없으면 0으로 제자리 덮어쓰고 그때 진단(값 2)한다. weak는 집합을 넓히지 않는다 |

- Include의 지연 덮어쓰기는 참조 wire가 U32 고정 폭이라 가능하다(Oe의 외부 번호 위치 표와 같은 방식 — GraphExternal.cpp:77-114). 그래서 노드 encode 순서가 결과를 바꾸지 않는다.
- 지연 항목은 (출력 위치, 대상 handle, objectIndex, 진단 경로 사본)이다. 경로 사본은 최대 `MaxPropertyPathSteps` 단계이고 1.6절 표기(삽입 순번)라 key를 복사하지 않는다. 그 메모리는 Oe 위치 표처럼 그래프 바이트 누계·작업량에 청구한다(GraphExternal.cpp:90-104). 진단은 모든 노드 encode가 끝난 뒤 기록 순서대로 한 번에 낸다(encode frame이 없어도 경로·objectIndex가 남는다).
- Admit hook 계약(OF 5.3절 예시 hook)에 `info.weak` 분기를 적는다. weak를 보지 않는 hook이 Import를 골라도 저장은 실패하지 않는다(위 표).
- 결정 21(RF 1023행): World의 parent·children을 리플렉션 WeakRef로 옮길지는 World 단계가 정한다. 이 단계는 가능하게만 한다.

### 5.2 ClassRef와 타입 도구 이름

- C++ 값 타입 `ClassRef<Base>`(ObjectRuntime `Object/ClassRef.h`): `TypeId id` 하나, 0 = null. `Base`는 리플렉션 클래스(아니면 RMC2003). 위치는 1.3절 표. SL `Class<T>`(SL 484행)가 이것이다.
- `value.type` = Base ID(필수, 의존 계수). wire Guid(7). `FieldValue.guid`.
- `ClassRef::Resolve(TypeRegistry&) -> Result<TypeLease>`: 등록·클래스·Base 파생을 다시 확인하고 lease를 준다. RCM 2단계의 등록 핸들(세대·revision 캐시 — RCM 30행)은 이 위에 RCM 2단계가 더한다.
- Required 허용(non-null).
- 검사 시점:

| 시점 | 검사 | 실패 |
|---|---|---|
| 쓰기(FieldValue·경로·텍스트) | 등록됨, 클래스(struct·enum 아님), `IsA(id, Base)`, Deprecated 아님(B9h2가 더한다) | InvalidArgument(미등록은 NotFound) |
| encode | 쓰기 검사에서 Deprecated만 뺀다(C++가 직접 넣은 값을 검사하되, 로드한 값을 다시 저장할 수 있게 한다 — UE도 Deprecated 클래스를 가리키는 클래스 참조 값을 저장한다. 저장 불가 판정은 객체의 클래스로 한다, SaveContext.cpp:136-148) | 저장 실패 |
| decode 1단계 | 등록·클래스·Base 파생(TypeRegistry 조회 — ObjectRegistry 접근 아님). Deprecated 클래스면 값은 유지하고 `DeprecatedType` 진단 | 새 정책 `unknownClass`: Reject(기본, NotFound) 또는 ReportAndNull(`UnknownClassReference` 진단 뒤 null). Map 키·Set 원소는 늘 Reject(1.7절) |
| 생성(RCM 3단계) | 추상·생성 가능성 | RCM 37행의 구분 오류 |

- 추상 클래스는 담을 수 있다(UE TSubclassOf도 담는다). 생성 가능성은 생성 때 따로 본다(RCM 37행).
- Base 판정은 schema 사슬의 `TypeRegistry::IsA`(TypeRegistry.h:120)를 쓴다. 영속 값이 TypeId이므로 판정도 TypeId 사슬로 한다.
- **타입 도구 이름**: `asset-new <Type>`·`asset-list --type=<Type>`(AS 571·573행)·ClassRef 텍스트·InstancedStruct `_structType`·Sparks가 같은 이름 하나를 쓴다.
  - 문법: `[A-Za-z_][A-Za-z0-9_]*('.' [A-Za-z_][A-Za-z0-9_]*)*`(Sparks 모듈 경로 `Weapons.Sword` — SL 20행). `:`는 쓰지 않는다. `none`은 예약이다(대소문자 무시).
  - 기본값은 C++ 타입의 한정하지 않은 이름이다. 충돌하면 `ROCKS_CLASS(Name="…")`·`ROCKS_STRUCT(Name="…")`로 정한다.
  - 유일성: 대소문자 무시로 TypeRegistry 안에서 유일하다(등록 AlreadyExists). MetaCompiler는 모듈과 import 범위에서 RMC2203으로 먼저 잡는다.
  - TypeRegistry에 도구 이름 → TypeId 색인을 더한다(A0-Ob가 둔 TypeId·native 키 색인 자리 — OF 11절 N3).
- TypeId 텍스트: 입력은 32자 16진(rocksids.json 형식)과 36자 소문자 하이픈 형식을 모두 받고, 출력은 36자 하나다(같은 codec의 Guid·AssetId와 같다).

## 6. Optional(B9e)

- C++ `Optional<T>`(CORE/Optional.h — 저장소 + `hasValue_`, :200-201). 클래스 필드·struct 멤버에만 둔다(원소·키·값이면 그 컨테이너 쪽 코드 RMC2304·2305·2307 — 15절, SL 482행 "타입 인자에는 `?`를 쓸 수 없다", SL 483행 "`?`는 중첩되지 않는다").
- 내부: Bool·정수·실수·Enum·Guid·AssetId·Digest·String·Name·값 struct(1.3절). 참조·컨테이너·Optional·InstancedStruct·Text는 거부한다.
- schema: `value = {Optional, 0}`, `element` = 내부 형식.
- wire Optional(14): present byte + (set이면) 내부 wire·payload. UE도 present 플래그 + 값의 같은 배치다(18.5절).
- **누락과 unset은 다르다**: 필드가 Record에 없으면 기준 값이다(생성자가 set으로 만들었으면 set). unset은 present = 0으로 쓴다.
- decode 2단계: set이면 `ObjectAllocator()`로 `Emplace`한다(String·allocator struct 내부). 할당만 실패한다.
- struct 재귀: C++ 불완전 타입으로 이미 막히지만 MetaCompiler 순환 DFS(RMC2302)가 Optional 간선도 따라간다.
- GC 없음. Required 거부.
- 경로: 단일 값 내부면 FieldValue(`kind` = 내부 종류, `present`)로 읽고 쓴다(present = false가 reset). struct 내부면 aggregate이고, 경로 연산 `EmplaceOptional`(기본 생성)·`ResetOptional`, 멤버 경로 `opt.member`는 set일 때만 쓴다(아니면 NotFound + 사유 `OptionalUnset`).
- 차이 단위(A5): Optional 필드 통째(present와 값이 한 단위).
- 텍스트: 명령행은 대입 종류 `Unset`(`--unset=<경로 목록>` — 10.4절)으로 unset하고, 값 표기는 내부 표기다. Optional<struct>를 기본값으로 set하려면 `opt={}`를 쓴다. JSON은 null = unset이다(UE JSON import는 null 필드를 건너뛰어 unset으로 되돌릴 수 없다 — 18.5절).

## 7. Map·Set·ObjectRefMap(B9f)

### 7.1 Core 컨테이너(B9f1)

`Core/Containers/OrderedMap.h`에 `OrderedMap<K, V, Hasher, KeyEqual>`, `Core/Containers/OrderedSet.h`에 `OrderedSet<T, Hasher, KeyEqual>`을 둔다(사용자 결정 U6). 헤더 첫 주석에 "삽입 순서를 보존한다 — 키 정렬이 아니다"를 적는다. 지금 Core HashMap은 open addressing·tombstone이고 순서를 보존하지 않는다(CORE/Containers/HashMap.h:283-301·357·388).

- 구조: 조밀 항목 Array(삽입 순서) + open addressing 색인(u32 위치) + 죽은 항목 표시. 제거는 항목을 죽은 것으로 표시하고, 죽은 수가 산 수의 절반을 넘거나 성장할 때 압축한다(상환 O(1)).
- 같은 키 대입은 위치를 유지하고, 제거 뒤 다시 넣으면 끝에 붙는다(Sparks `m[k] = v` — SL 482행).
- 순서는 해시 값과 무관하다. 그래서 순회·직렬화가 해시 seed·플랫폼과 무관하게 결정적이다.
- API는 Core HashMap 규약을 따른다(HashMap.h:72·121-374, Docs/Implementation/HashCollections.ko.md): `explicit OrderedMap(Allocator&, Hasher = {}, KeyEqual = {})`, `Count`·`IsEmpty`·`Find`·`Contains`, `Insert -> Result<InsertOutcome>`(`AlreadyPresent`), `InsertOrAssign`, `FindOrAdd`, `Reserve`, `Compact`, `Erase`, `Clear`·`Reset`, `ForEach`·반복자. 할당은 `Result`, 예외 없음. Name 키는 Core `Hash<Name>`(Hash.h:83)을 쓴다.

### 7.2 리플렉션 Map·Set(B9f2)

- PROPERTY: `OrderedMap<K,V>`·`OrderedSet<T>`. `HashMap`·`HashSet` PROPERTY는 RMC2305(순서 없는 컨테이너는 저장할 수 없다).
- 키·원소: 정수 8종·Bool·Enum·Guid·AssetId·Digest256·String·Name·SoftObjectRef·ClassRef. 값: 1.3절 Map 값 열(ObjectRef는 7.4절 ObjectRefMap).
- schema: Map은 `value = {Map, 0}`, `key`, `element` = 값, `maxElements`. Set은 `value = {Set, 0}`, `element`. 키 대상(Enum 레코드·ClassRef Base·Soft 기대 타입)은 1.1절 키 대상 칸에 둔다.
- wire Map(16)·Set(15)(1.2절). 순서 = 삽입 순서.
- decode: 1단계가 중복 키를 찾는다(임시 색인은 1단계 예산에 청구). String은 byte, Name은 `TryMakeNameKey` 해시와 `NameTextEquals`로 비교한다. 중복은 InvalidArgument. 키 위치는 값 대체 정책을 쓰지 않는다(1.7절). 2단계는 파일 순서대로 컨테이너를 만든다.
- 한도: `MaxElements`는 쓰기·encode에서 걸고, decode는 `MaxArrayElements`와 그래프 한도만 쓴다(RF 결정 13과 같다).
- Required: Map 값이 Soft·ClassRef면 값마다 non-null(배열과 같은 뜻). Set 원소가 Soft·ClassRef면 원소마다 non-null. 키에는 Required가 없다.
- GC: 값 컨테이너는 없다.
- 차이 단위(A5): 필드 통째(배열 규칙 — RCM 64행). UE는 Map·Set을 key·원소 단위로 차이 저장한다(18.7절). key·원소 단위 차이는 A5·prefab의 후보로 남긴다(RCM 64행 "안정 키 컨테이너에만 후속 허용" — Set도 포함). 같음은 삽입 순서까지 포함한 byte 비교다(순서는 Sparks 순회로 드러나는 상태다).
- 텍스트: 10.4절(명령행 `k:v` 목록, JSON 객체·배열).

A5 `Downscale` 플랫폼별 값(AS 451행 계약):

- `PerPlatformFloat { float defaultValue; ROCKS_PROPERTY(EditorOnly) OrderedMap<Name, float> perPlatform; }`꼴 struct로 표현한다. UE FPerPlatformFloat와 같은 모양이다(PerPlatformProperties.h:224-239 — PerPlatform 맵은 editor 전용). 키는 TargetProfile id(AS 301행)이고, Name이라 대소문자 변형에 안전하다.
- Downscale은 텍스처 빌드 입력이라 쿠킹 데이터에 쓰이지 않는다. A5 처리기가 대상 프로파일 값으로 해석해 빌드에 쓴다. 쿠킹 데이터에 접어 넣을 필드가 생기면 B4 `SubstituteStruct`(struct 치환 hook — RF 6.1절, AS 220행)로 접는다. UE의 플랫폼 그룹 조회(PerPlatformProperties.h:82-108)는 TargetProfile 그룹이 생길 때 후속이다. struct 정의는 A5가 둔다.

### 7.3 경로 key 단계(B9f3)

- 1.5절 표현. Map 연산: `m[key]` 읽기·쓰기(삽입 또는 교체 — SL 482행), `Erase`, `Clear`, `Count`, `Contains`. 값이 struct면 `m[key].member`와 기본값 삽입·초기화 `InsertDefault`(텍스트 `m[k]={}`).
- Set 연산: `Insert`·`Erase`·`Contains`·`Clear`. 원소는 키라 멤버 경로가 없다. Set 원소는 `s[원소]` key 단계로 가리킨다.
- Name 키 조회는 `NamePool::TryFind`로 토큰을 얻는다. pool에 없는 이름이면 그 키도 없다(NotFound, 할당 없음).
- 쓰기 규칙은 B6과 같다: 할당은 게시 전, 게시는 무할당, 옛 값을 돌려준다(RF 결정 20).

### 7.4 ObjectRefMap(B9f4)

사용자 결정 U3으로 B9에 포함한다.

- `ObjectRefMap<K, T>`: ObjectRefArray(OR/ObjectRefArray.h:14-61)처럼 저장소를 ObjectRegistry만 바꾼다. 키 종류는 7.2절과 같다. 값은 `ObjectRef<T>` 의미(null 허용 — null 값은 간선이 아니다).
- 변경 API(ObjectRegistry): `SetMapReference(owner, map, key, target)`(삽입 또는 교체), `EraseMapReference(owner, map, key)`, `ClearMapReferences(owner, map)`. non-null 대상은 삽입 장벽·shade(Marking 중), 대상 타입·생존·같은 Registry 검증, 관찰자 계수(편집은 Registry API, Commit·Discard는 trace — RuntimeTypeOps.h:391-397와 같은 규칙). 실패 시 내용 불변.
- staged API(그래프 로드): `ConstructionContext::AssignMapReferences(handle, ObjectRefMap<K,T>&, Span<const K>, Span<const ObjectHandle>)`. 소유자 검사·대상 규칙은 `AssignReferences`(RF 5.3절)와 같다.
- 생성 Trace가 값을 방문한다. Commit의 staged 간선 검증도 Trace로 값을 방문한다.
- Required: 값마다 non-null.
- 위치: 클래스 필드·인라인 struct 멤버(ObjectRefArray와 같다 — RF 4.3절). StructRefArray 원소 struct에는 둘 수 없다(RF 결정 15와 같은 이유).
- 경로 연산: `m[key]` 읽기·쓰기·`Erase`·`Clear`(7.3절, B6 쓰기 규칙). 대입 묶음 게시는 10.2절의 관찰자 예약을 거친다.
- AS 6.1절 관찰 대상 목록(AS 233행)에 ObjectRefMap 값을 더한다(21절).

## 8. 정적 배열(B9g)

- PROPERTY `T name[N]`. N은 1..`MaxStaticArrayElements`(65,535 — UE 생성 코드의 uint16 ArrayDim과 같은 상한, UObjectGlobals.h:3753). 다차원은 RMC2307.
- 원소: 1.3절 StaticArray 열. 원소가 소유 객체 저장소 안에 있으므로 `ObjectRef<T> refs[4]`와 참조 struct 원소도 Registry 컨테이너 없이 된다(SetReference의 주소 범위 검사를 통과한다 — RF 4.3절과 같은 이유). bool 원소도 허용한다.
- schema: `value = {StaticArray, 0}`, `element`, `maxElements = N`. ObjectRef 원소면 `FieldKind::ObjectReference`.
- wire: 단일 값 원소는 ValueArray, struct 원소는 RecordList, ObjectRef 원소는 ValueArray of U32. encode는 늘 N개를 쓴다.
- decode: count가 N이 아니면 InvalidArgument. N을 바꾸면 Version을 올리고 migrate한다(차원은 배치다. 조용한 잘림을 만들지 않는다).
- 경로: `[i]`(0..N-1) 읽기·쓰기·멤버. 삽입·삭제·비우기는 Unsupported. `MaxElements` 지정자는 RMC2202.
- GC: ObjectRef 원소마다 방문, 참조 struct 원소는 그 Trace.
- NATIVE 형질: `NativeStrongReference<T[N]>`·`RegistryContainer<T[N]>` 특수화를 더하고(OR/NativeMemberTraits.h:16-70), MetaCompiler `StrongReferencePath`가 C 배열을 따라간다. 그래야 생성 static_assert가 배열 멤버에서도 강한 참조를 잡는다.
- Required: 참조·Soft·ClassRef 원소면 원소마다 non-null.
- 차이 단위(A5): 통째(배열 규칙). UE는 index마다 태그다(18.8절). 원소별 차이는 A5·prefab 후보다.
- 텍스트: 정확히 N개의 쉼표 목록, JSON 배열 길이 N.
- **배열 원소 확장**: Name(B9c2)·WeakRef(B9d1)·ClassRef(B9d2)·Int8·Int16(B9a2)·InstancedStruct(B9j)·Text(B9l)는 각 종류 단계가 Array·StaticArray 원소와 Map 값까지 함께 넣는다(B9f·B9g보다 앞 단계면 B9f2·B9g가 그 종류의 Map 값·StaticArray 원소를 넣고, 뒤 단계면 그 종류 단계가 넣는다). B9g는 정적 배열과 "성질 표 = 실제 지원" 대조 시험을 맡고, 이 대조 시험은 뒤 종류 단계마다 행을 늘린다. RF 1036행의 "WeakRef·클래스 참조·FName·FText 원소"가 이것으로 닫힌다.

## 9. 필드·클래스 플래그(B9h)

### 9.1 필드 플래그(B9h1)

| 지정자 | 비트 | 의미 | 쓰는 곳 |
|---|---|---|---|
| `DuplicateTransient` | 16 | 복제(에셋 복제·PIE 복제) 저장과 복사 텍스트에서 뺀다 | A2 `asset-duplicate`(AS 97행), PIE, ReflectionText Copy |
| `NonPIEDuplicateTransient` | 32 | PIE가 아닌 복제와 PIE가 아닌 텍스트 출력에서 뺀다(예: 인스턴스 Guid) | A2, ReflectionText Edit·Copy |
| `SaveGame` | 64 | 세이브게임 저장은 이 플래그가 있는 필드·멤버만 쓴다 | 세이브게임 계층(후속) |
| `TextExportTransient` | 128 | 복사 텍스트에서 뺀다 | ReflectionText Copy |

- **복제 저장**: 복제 호출자가 `excludedFlags`를 정한다. 에셋 복제 = DuplicateTransient | NonPIEDuplicateTransient, PIE 복제 = DuplicateTransient. 복제본 로드는 정책 없이 읽는다(빠진 필드 = 기준). 전부 저장(RCM 65행)의 override 집합 기록에서도 이 필드들의 경로를 뺀다.
- **포함 필터** `includeFlags`(`FieldEncodePolicy`·`FieldDecodePolicy`, 0이면 끔): 0이 아니면 **필드·struct 멤버마다 자기 플래그**에 이 비트가 없으면 쓰지 않는다(UE처럼 멤버마다 표시 — 사용자 결정 U5, PR:1051-1055와 UT:543). SaveGame struct 필드의 멤버도 각각 SaveGame이어야 쓰인다. 배열 원소·Map 값·Optional 내부·InstancedStruct 내용의 struct 멤버에도 같은 규칙을 쓴다.
  - 예외: Atomic struct 안의 멤버에는 적용하지 않는다(통째). UE에서도 native Serializer 수학 타입은 멤버 플래그를 보지 않고(PR:53-68), immutable 수학 타입은 멤버가 모두 SaveGame이다(NET:2080-2088 등). 비Atomic `TransformBinding`은 멤버에 SaveGame을 단다(3.2절).
  - 로드 쪽 `includeFlags`: 포함 비트가 없는 필드·멤버가 파일에 있으면 InvalidArgument(excludedFlags와 대칭). 2c Required 검사도 포함 비트 밖의 최상위 필드·멤버를 뺀다(검사 문맥의 `IsExcluded`가 includeFlags를 멤버마다 적용한다 — GraphRequired.cpp:28 검사 정책에 includeFlags를 더한다).
  - RF 결정 22·RCM 65행의 "세이브게임은 전부 저장"은 "기준 차이 없이 모든 값"이라는 뜻이다. 필드 범위는 SaveGame이 정한다(21절에서 문구를 더한다).
- **Required와의 조합**: Required와 DuplicateTransient·NonPIEDuplicateTransient를 함께 쓰면 RMC2202다. 복제본이 늘 null을 가져 패키지 저장(`rejectMissingRequired`)이 실패하기 때문이다.
- 2c Required 검사는 로드 정책의 excludedFlags에 든 필드를 지금처럼 뺀다(GraphRequired.cpp:28).

### 9.2 클래스 플래그(B9h2)

| 지정자 | 비트·칸 | 의미 |
|---|---|---|
| `Transient` | TypeFlags 4, 상속 | 패키지 저장(저장 정책 Exclude) 때 이 클래스 객체를 뺀다. 로드는 되고 진단을 낸다 |
| `Deprecated` | TypeFlags 8, 상속 | 패키지 저장(저장 정책 Exclude) 때 이 클래스 객체를 뺀다(UE SavePackage와 같다 — 사용자 결정 U5). 로드는 되고 진단을 낸다 |
| `Within="T"` | TypeSchema.withinType | Outer가 T 또는 파생이어야 한다 |

**저장 규칙(Transient·Deprecated 공통 — 저장 불가 객체, 1.7절)**

UE에서 저장 불가 판정은 패키지 저장(SavePackage)의 규칙이다. 복제(StaticDuplicateObject)는 클래스 플래그로 거르지 않고, `FDuplicateDataWriter`는 객체 플래그 RF_DuplicateTransient·RF_NonPIEDuplicateTransient만 본다(DuplicateDataWriter.cpp:69-79). 그래서 처리를 저장 옵션으로 나눈다.

```cpp
enum class UnsaveablePolicy : uint8 {
    Exclude, // 패키지 저장(A1) — UE SavePackage와 같다. 기본값
    Keep     // 스냅샷·undo·PIE 복제 — UE 복제와 같다. 저장 불가 객체도 다른 객체처럼 저장한다
};
// ObjectGraphSaveOptions::unsaveable = UnsaveablePolicy::Exclude
```

| 정책 | 저장 집합에 저장 불가 객체 | 그 객체를 가리키는 참조 | 편의 저장 Include |
|---|---|---|---|
| Exclude(기본) | InvalidArgument(저장 집합은 호출자가 정한다 — OF 5.1절, 호출자가 미리 거른다) | 강한·weak 모두 외부 정책과 무관하게 null + `ExternalReference` 진단(값 3 = Transient 클래스, 4 = Deprecated 클래스). Required 위치면 null에 Required 규칙(`rejectMissingRequired`) | 집합에 넣지 않고 null + 진단. root(노드 1)가 저장 불가면 InvalidArgument |
| Keep | 보통 노드로 저장 | 보통 참조 | 보통 대상 |

- 기본값을 Exclude로 둔 이유: 영속 산출물(패키지)이 실수로 저장 불가 객체를 담지 않게 하려는 것이다. 스냅샷·undo·PIE 복제 호출자는 Keep을 명시한다.
- A1 export 집합 구성은 `CollectInners` 결과를 `IsUnsaveableObject`로 걸러 저장 불가 객체와 그 부분 트리를 뺀다. `CollectInners`(ObjectRegistry 공개 API) 자체는 바꾸지 않는다. UE도 저장 불가 Outer의 inner를 함께 뺀다(SaveContext.cpp:95-110).
- 로드: Transient·Deprecated 클래스 노드 모두 정상 로드하고 진단(`TransientType`·`DeprecatedType`)을 낸다. UE 로드 경고와 같다(LinkerLoad.cpp:6122-6131 Deprecated(편집기), 6146-6154 Transient(CDO·archetype 제외)). 그래서 클래스를 나중에 Transient로 바꿔도 기존 패키지를 열고 고칠 수 있다. Exclude로 다시 저장하면 그 객체와 그 참조는 빠진다. Keep으로 만든 스냅샷을 복원할 때도 같은 진단이 나며, 받을 hook이 없으면 하지 않는다.
- 생성: Transient·Deprecated 클래스의 C++ `NewObject`는 막지 않는다(UE도 막지 않는다). ClassRef 쓰기·텍스트 대입·편집기 후보(RCM 5단계)는 Deprecated 클래스를 거부한다. 이미 저장된 ClassRef 값은 그대로 로드(진단)·저장한다(5.2절).
- 나중에 A5 클래스 기본값·prefab(archetype)이 생기면 Transient 클래스의 템플릿 객체 예외(UE는 CDO·archetype을 뺀다 — UOG:3739-3743, LinkerLoad.cpp:6146)를 그 단계에서 다시 정한다.

**MetaCompiler 규칙**

- Deprecated가 아닌 타입의 PROPERTY가 Deprecated 클래스를 가리키면(`ObjectRef<D>`·`WeakRef<D>`·`SoftObjectRef<D>`·`ClassRef<D>`와 그 컨테이너·Map 키·값) RMC2209다. UE도 Deprecated 클래스를 쓰는 비Deprecated 속성을 오류로 막는다(UhtProperty.cs:2612-2631, UhtObjectPropertyBase.cs:752-756). Rocks에는 필드 Deprecated가 없으므로 그 필드를 retire하고, 옛 데이터가 필요하면 Version과 migrate로 옮긴다. Deprecated 타입 자신의 필드와 NATIVE 멤버는 검사하지 않는다.
- Transient는 상속되고 끄는 지정자는 없다(UE `NonTransient` 대응 없음 — 20절 결정 15).

**Within**

- 지정하지 않으면 부모의 Within을 상속하고, 지정하면 부모 Within의 파생이어야 한다(RMC2208). 자기 자신을 Within으로 둘 수 있다(UE와 같다 — UhtClass.cs:1442).
- 값 해석: `Within="…"`의 철자는 Binds와 같은 방식으로 Clang이 정규 이름으로 푼다(`using rocksWithin_N = ::철자;` 확인 실행 — MC/CompilerDriver.cs:534-589와 같은 경로).
- 판정 위치: `OuterSummary`(OR/ObjectRegistry.h:48-66)에 Outer 타입(TypeId)을 더하고, 생성 정체성 규칙(`detail::CheckNewObjectIdentity` — Registry 생성·그래프 1단계·2a 사전 확인이 함께 쓰는 순수 함수, ObjectRegistry.h:84-100)에 Within 단계를 더한다(패키지 규칙 다음). 실패는 InvalidArgument다.
  - 그래프 안 Outer(노드 k)는 노드 표로 타입을 알므로 1단계에서 판정한다(파일 손상).
  - outer 0(기준 Outer)은 2a가 실제 handle로 판정한다.
  - `OuterSummary::ForNewOutermost`에 새 outermost의 타입 칸을 더한다(A1 로더의 새 Package).
  - Outer 없는 객체에 Within이 있으면 InvalidArgument다(UE에서 Outer 없는 객체는 패키지뿐이다 — UOG:3481).
- Outer는 생성 뒤 바뀌지 않으므로(OF 1.1절) 생성 검사로 충분하다. UE는 Rename·PreSave에서도 검사한다(Obj.cpp:270·1622-1631).

## 10. 텍스트·JSON codec(B9i)

### 10.1 모듈과 용도

- 새 모듈 `ReflectionText`(Core/Source/Runtime, ObjectRuntime·Core 의존)를 리플렉션 공용 API로 둔다. RF 650행 "명령행 값 표기는 도구 계층"을 정정한다. ObjectRuntime 안에 두지 않는 이유는 JSON이 ObjectRuntime의 모든 사용자에게 끌려가지 않게 하려는 것이다.
- Core 공용 기능을 쓴다: JSON 읽기·쓰기(CORE/Text/Json.h), 정수·실수 파싱(CORE/Text/NumberText.h:28-41), 최단 실수 표기(Core `StringBuilder`의 `"{}"` — 이미 float·double 최단 왕복이다), Unicode NFC, 목록 분해(Core StringSplit).
- B9i1 Core 추가:
  - `JsonWriter::Float(float)`: Canonical 규칙의 단정도 최단 표기. 지금은 `Double`만 있어 float 필드가 넓힌 double 표기(0.1f → 0.10000000149011612)로 나간다. JSON 읽기는 `JsonValue::RawNumber()`를 `TryParseFloat`로 읽어 이중 반올림을 피한다.
  - `JsonValue::Position()`: 노드 시작 byte offset과 그로부터 계산한 line·column. 의미 오류(모르는 키·타입 불일치·범위)의 위치 보고에 쓴다.
  - Core StringSplit에 escape 인식 분해 옵션(`\,`·`\;`·`\:`). A2 명령행 파서와 공유한다.
- **용도(`TextUse`)** — 기본 제외 플래그를 고른다. UE `ShouldPort`(PR:1161-1182)와 JsonObjectConverter(JsonObjectConverter.cpp:404-410)를 따른다.

| 용도 | 출력 제외 | 입력 제외 | 쓰는 곳 | UE 대응 |
|---|---|---|---|---|
| `Edit` | Transient · NonPIEDuplicateTransient | Transient | `asset-set`·`asset-new` 입력, `asset-info` 기본 출력, 설정 JSON(TargetProfile·CookSettings) | 출력은 `ShouldPort`(Copy·PIE 아님 — PR:1166-1170·1178-1182). 입력은 속성 편집처럼 `ShouldPort`를 거치지 않는다 |
| `Copy` | Edit 출력 + DuplicateTransient · TextExportTransient | 출력과 같다 | 복사·붙여넣기, 다른 에셋에 값 옮기기 | PPF_Copy(PR:1172-1176) |
| `PieCopy` | Transient · DuplicateTransient · TextExportTransient | 출력과 같다 | PIE 세션의 복사 | PPF_Copy + PPF_DuplicateForPIE |
| `Report` | Transient | 입력에 쓰면 InvalidArgument | `asset-info --all`, 검증 보고 JSON | JsonObjectConverter 기본 SkipFlags |

- 입력 제외 필드에 값을 주면 진단 `ExcludedProperty`로 거부한다(엄격 — UE는 경고 후 건너뛴다, PR:1664-1667). Edit 입력은 NonPIEDuplicateTransient·TextExportTransient 필드를 받는다(인스턴스 고유 값도 도구가 고칠 수 있다. UE에서 이 둘의 제외는 텍스트 출력 규칙이다).
- ReadOnly 필드: 살아 있는 객체 대상이면 진단 `ReadOnly`, staged 객체(생성 중 — `asset-new`) 대상이면 받는다(ReadOnly는 생성·복원의 staged write를 허용한다 — MS/TypeSchema.h FieldFlags 주석). 살아 있는 객체 대상이라도 값이 지금 값과 같은 ReadOnly 대입은 무동작으로 받고 override 경로에도 넣지 않는다(`asset-info` 출력의 재입력 — 10.4절).
- 숨김 열거자(`EnumeratorFlags::Hidden`): 입력은 진단 `HiddenEnumerator`로 거부한다(A5가 `InvalidSetting`으로 바꾼다 — AS 435행). 출력은 그대로 쓴다.

### 10.2 대입 묶음 API

```cpp
namespace rocks::reflection_text {
enum class AssignmentKind : uint8 { Set, Unset, Erase };
struct TextAssignment {
    AssignmentKind kind = AssignmentKind::Set;
    StringView path;          // 도구 이름 경로(1.5절)
    StringView value;         // Set만
    uint32 sourceIndex = 0;   // 진단에 쓰는 입력 순번
};
// 대상: 살아 있는 객체(registry, access, handle) | staged 객체(ConstructionContext&, handle) | struct 값(가변 StructView)
class AssignmentTarget;
enum class MapMergeMode : uint8 { Replace, MergeByKey };
inline constexpr uint32 DefaultMaxTextDiagnostics = 256;
struct TextCodecOptions {
    TextUse use = TextUse::Edit;                      // 출력·입력 제외 플래그(10.1절)
    TextReferenceResolver* resolver = nullptr;        // 10.3절. 없으면 none·36자 AssetId만
    NamePool* names = nullptr;                        // Name intern·조회(대상 Registry의 pool과 같아야 한다)
    MapMergeMode mapMerge = MapMergeMode::Replace;    // JSON import(10.4절)
    uint32 maxDiagnostics = DefaultMaxTextDiagnostics;
    // 입력 byte·JSON 깊이·원소 수 한도
};
class PreparedAssignments {
public:
    // 앞 대입이 만든 상태 위에서 해석한다. 같은 경로 두 번은 DuplicateAssignment. 진단은 Prepare에 준
    // TextDiagnostics로 간다(그 객체는 PreparedAssignments보다 오래 살아야 한다). resolver도 여기서 부른다.
    Result<void> Append(const TextAssignment&) noexcept;
    // override 단위로 정규화한 경로(중복 없음): Atomic struct는 struct 경로, Map·Set·Array·StaticArray·
    // Optional·InstancedStruct는 필드 경로, 그 밖은 끝 경로.
    Span<const PropertyPath> OverridePaths() const noexcept;
    bool Touches(PropertyPath unit) const noexcept;
    // 한 번만. 성공하면 게시한 override 경로(값이 바뀌지 않은 대입 포함)와 옛 값을 돌려준다.
    Result<PublishedAssignments> Publish() noexcept;
};
Result<PreparedAssignments> PrepareAssignments(AssignmentTarget, Span<const TextAssignment>,
                                               const TextCodecOptions&, TextDiagnostics&) noexcept;
}
```

- **두 단계**: Prepare(해석·검증·할당·resolver 호출) → 들여다보기·덧붙이기(`Touches`·`Append`) → Publish. A5의 "같은 명령에서 SRGB를 주지 않았으면 SRGB off를 함께 저장"(AS 408행)은 `Touches(SRGB)`가 거짓이면 `Append("SRGB=off")`로 한다.
- **묶음 의미**:
  - 같은 경로를 두 번 대입하면 `DuplicateAssignment`다(AS 600행 "같은 옵션 두 번"과 같다). 다른 경로는 겹쳐도 된다(`m=a:1` 뒤 `m[a]=2`).
  - 뒤 대입은 앞 대입이 만든 상태 위에서 해석한다. 대입이 닿는 위치마다 아래 준비 단위를 만들고 대입을 차례로 그 단위에 적용한다. 그래서 `Inst=<타입>` 뒤의 `Inst.m=1`, `m[new]={}` 뒤의 `m[new].x=1`, `opt={}` 뒤의 `opt.m=1`이 된다.
  - Prepare는 경로·값 문자열을 복사한다(입력 `StringView`는 호출 동안만 빌린다).
- **준비 단위** — 참조 쓰기의 삽입 장벽·관찰자 기록·소유자 주소 범위 검사(RF 4.3절)를 우회하지 않는다:
  - 참조 없는 값 부분 트리(단일 값·값 struct·값 컨테이너·Optional·InstancedStruct·Text·Name·WeakRef): 사본(`TryClone`)을 고치고 게시 때 옮긴다. 인라인 struct 필드가 참조 멤버를 섞어 가지면 struct 전체가 아니라 참조 없는 멤버마다 이 단위로 나눈다.
  - 단일 참조 위치(ObjectRef 필드, 인라인 struct의 ObjectRef 멤버, StaticArray의 ObjectRef 원소, StructRefArray 원소의 참조 멤버): (위치, 옛 대상, 새 대상) 간선 변경으로 두고, 게시 때 그 위치에 제자리로 쓰며 삽입 장벽·관찰자 기록을 적용한다.
  - Registry 컨테이너(ObjectRefArray·StructRefArray·ObjectRefMap): 새 버퍼와 간선 차이(더한 대상·뺀 대상)로 두고, 게시 때 버퍼를 바꾸고 더한 대상에 장벽을, 차이에 관찰자 기록을 적용한다.
  - Publish 2단계가 예약할 관찰자 키는 간선 변경·간선 차이에서 센다.
- **대상 고정**: Prepare와 Append는 묶음이 새로 가리키는 non-null Hard·Weak 대상을 `RootRef`(`TryPin`)로 붙잡고, Publish가 끝나거나 `PreparedAssignments`가 소멸할 때 놓는다. 준비 단위는 GC Trace 밖이라 붙잡지 않으면 그 사이 GC가 대상을 회수할 수 있다. 고정은 Discard를 막지 못하므로 Publish 2단계가 다시 확인한다.
- **Publish(살아 있는 객체)**:
  1. 소유자 Borrow를 얻고, Prepare 뒤 소유자가 바뀌지 않았는지 객체 변경 표지(`ObjectRegistry::MutationStamp(handle)` — 리플렉션 쓰기·Registry 참조 변경마다 증가)로 확인한다. 바뀌었으면 Busy, 소유자가 죽었으면 StaleHandle이고 아무것도 바뀌지 않는다.
  2. Registry 묶음 게시 구간(`ObjectRegistry::BeginBatchPublish`)에 들어간다. 구간 안에서는 그 Registry의 다른 변경·GC 단계·Discard가 Busy이고 콜백이 없다. 그 안에서 (가) 모든 새 대상(Hard·Weak)의 생존(Discard·회수 아님)·기대 타입·같은 Registry를 다시 확인하고, (나) 간선 변경에서 센 관찰자 키 공간을 예약한다(`ReferenceObserver::Reserve` — OR/ReferenceObserver.h:108-113. 처리 표시 아래 잠금 밖에서 예약한 뒤 다시 잠가 확인하는 지금 `ClearReferences` 방식 — ObjectRegistry.h:441-447). 어느 것이든 실패하면(StaleHandle·TypeMismatch·OutOfMemory·CapacityExceeded) 아무것도 바뀌지 않는다.
  3. 준비 단위를 무할당으로 차례로 게시한다. 2에서 대상과 공간을 확인했고 구간 안에는 다른 변경·콜백이 없으므로 이 단계는 실패하지 않는다. 삽입 장벽의 shade는 미리 잡은 GC 작업 용량을 쓰는 무할당 연산이고, 그 용량이 모자라는 것은 불변식 위반이다(ORP/ObjectRegistryGc.cpp:79-83).
  4. 구간을 나온 뒤 옛 값·옛 버퍼를 반환하고 대상 고정을 놓는다.
  - 지금 `SetReference`·ObjectRefArray 변경은 게시 시점에 대상 검증(StaleHandle·TypeMismatch)·관찰자 예약 실패가 날 수 있다(ObjectRegistry.h:407-447). 이 실패를 2단계로 앞당기는 것이 B9i2 커밋의 선행 변경이다.
  - 계약: Prepare와 Publish 사이에 C++ 코드가 같은 객체의 값 필드를 직접 바꾸면 변경 표지에 잡히지 않고 게시가 그 값을 덮는다. 표지는 리플렉션 쓰기와 Registry 참조 변경만 올리며, 그 사이 직접 쓰기는 호출자 책임이다.
- **staged 객체**(`asset-new`): staged write로 게시하고 관찰자가 없다. 실패하면 호출자의 트랜잭션 rollback이 원자성을 준다.
- **struct 값**: 준비 사본을 대상 struct에 무할당으로 옮긴다(참조를 담는 struct 값은 대상이 될 수 없다 — 참조 struct는 객체 안에서만 쓴다).
- **진단**: `TextDiagnostic { code; uint32 sourceIndex 또는 JsonPosition; 경로 텍스트; 설명 }`. 코드: UnknownProperty·ExcludedProperty·ReadOnly·InvalidValue·OutOfRange·DuplicateKey·DuplicateAssignment·HiddenEnumerator·UnknownType·Unresolved(resolver 실패)·Unsupported(resolver 없음). Prepare는 한도(`maxDiagnostics`) 안에서 오류를 모두 모아 보고하고 아무것도 게시하지 않는다(사람이 고치는 설정 파일에 맞는 방식).

### 10.3 참조 resolver

```cpp
enum class TextReferenceKind : uint8 { Hard, Weak, Soft };
class TextReferenceResolver {
public:
    // Hard·Weak: 같은 Registry의 살아 있는 handle. Soft: AssetId와 객체 경로만(로드하지 않는다 — AS 6.2절).
    virtual Result<ResolvedReference> Resolve(StringView text, TypeId expected, TextReferenceKind) noexcept = 0;
    // handle·Soft → 논리 경로. 실패하면 codec이 36자 AssetId로 대신 쓴다.
    virtual Result<void> Format(const ReferenceValue&, StringBuilder<>& out) noexcept = 0;
};
```

- 호출 시점: Prepare·Append 안에서만, Borrow·encode 고정을 쥐지 않은 상태에서 부른다. 해석한 Hard·Weak 대상은 곧바로 고정한다(10.2절). Hard 해석이 대상 패키지 로드를 일으켜도 묶음 원자성과 무관하다.
- 실패는 진단 `Unresolved`로 옮긴다. resolver가 없으면 `none`과 36자 AssetId(빈 객체 경로 Soft)만 받고 non-null Hard·Weak는 `Unsupported`다.
- A1·A2가 논리 경로·AssetId로 구현한다.

### 10.4 표기

명령행 escape 표(codec 전체에 하나): 목록 구분자 `,`, 목록 안 `\,`·`\;`, Map 항목은 **처음 나오는 escape되지 않은 `:`에서 나눈다**(키 안의 `:`만 `\:` — 값 안의 `C:\x`는 escape 없이 쓴다), 경로 key 따옴표 안 `\"`. 그 밖의 `\`는 글자 그대로다(AS 13절 597행 규칙을 넓힌다). 대입 `<경로>=<값>`은 `[...]`와 따옴표 밖의 첫 `=`에서 나눈다(String 키 `a=b`는 key 따옴표로 쓴다 — 1.5절).

| 종류 | 명령행 | JSON |
|---|---|---|
| bool | `on`·`off`·`true`·`false`(출력 `true`·`false`) | true·false |
| 정수 8종 | 10진, 범위 검사 | 정수(int64·uint64 그대로 — Json 파서가 보존) |
| 실수 | 10진, 출력 최단 왕복, `nan`·`inf`·`-inf` | 수(float는 `JsonWriter::Float`), 비유한은 문자열 `"nan"`·`"inf"`·`"-inf"` |
| enum | 도구 이름(접두 선택, 대소문자 무시) | 같은 문자열 |
| Flags enum | `A+B`(입력은 `A|B`도 받는다), 0은 빈 값 | 이름 배열 `["A","B"]` |
| Guid·AssetId | 36자 소문자 | 문자열 |
| Digest256 | 64자 소문자 16진 | 문자열 |
| String | 그대로 | 문자열 |
| Name | 그대로(NFC로 정규화 후 intern), None 출력 `None` | 문자열 |
| ObjectRef·WeakRef·Soft | resolver, null은 `none` | 문자열·null |
| ClassRef | 타입 도구 이름·TypeId, null은 `none` | 문자열·null |
| Array·StaticArray·Set | 쉼표 목록(빈 값 = 빈 배열) | 배열 |
| Map | `k:v` 쉼표 목록 | 객체(키 텍스트 표기, 삽입 순서) |
| Optional | 대입 종류 Unset(`--unset=<경로 목록>`), set은 내부 표기, struct 내부는 `opt={}` | null = unset |
| struct | 경로 대입 `<f>.<m>=v`, 기본값으로 `<f>={}` | 객체(키 = 필드 도구 이름) |
| struct 원소 컨테이너 | `a=`(비움), `a[i]={}`(i = 길이면 끝에 기본 원소 추가, 작으면 기본값으로 초기화), `m[k]={}`(기본값 삽입·초기화), 이어서 멤버 경로 | 배열·객체 |
| InstancedStruct | `<f>=<타입 도구 이름>`(형 바꾸기, 기본값), `<f>.<m>=v`, 비움은 `none` | `{"_structType": "<타입 도구 이름>", …}`, 비어 있음은 null(입력은 `{}`도 받는다) |
| Text | Invariant 원문(명령행은 Invariant만 — 사용자 결정 U7) | 문자열 = Invariant, 객체 `{"id","revision","source"}` = Localized, null = 비어 있음 |

- 명령행 대입 종류: `<경로>=<값>`(Set), `--unset=<경로 목록>`(Optional unset), `--erase=<경로 목록>`(Map 키·Set 원소 제거 — `Downscale.perPlatform[Android]`). 목록 옵션이라 "같은 옵션 두 번"에 걸리지 않는다. AS 13절 문법을 함께 고친다(21절).
- `none`은 참조·ClassRef·InstancedStruct의 null 전용이다. 논리 경로(`/`로 시작)·36자 AssetId·타입 도구 이름(`none` 예약)과 겹치지 않는다. enum 열거자 `None`·Name `None`과는 종류가 달라 모호하지 않다.
- JSON 객체 키는 필드 도구 이름이고 대소문자 무시로 맞춘다(경로 규칙 — RF 647행). 대소문자만 다른 두 키는 `DuplicateKey`다. 모르는 키는 `UnknownProperty`다(UE는 strict 모드에서도 모르는 키를 허용한다 — 18.10절).
- JSON import 의미(기존 값 위에 읽을 때):

| 종류 | 의미 |
|---|---|
| struct | 멤버 재귀 병합(없는 키는 지금 값 유지 — 누락 = 기준과 같은 뜻) |
| Array·StaticArray·Set·Optional·InstancedStruct·Text | 통째 교체 |
| Map | 기본 통째 교체. 옵션 `mapMerge = MergeByKey`면 JSON에 있는 키만 병합(struct 값은 재귀 병합)하고 없는 키는 유지한다 |

- A4 TargetProfile의 프로젝트 재정의(AS 301행)가 그룹 단위 덮기를 원하면 `OrderedMap<TextureGroup, LODGroup>` 모델과 `MergeByKey`를 쓴다(UE는 `*TextureLODGroups=Group` 배열 키 병합 — A4가 고른다).
- **`asset-info` 출력**: 기본은 Edit 용도의 `<경로>=<값>` 줄이다. 그 출력을 그대로 `asset-set`에 넣으면 값이 보존된다(값 보존 왕복 — ReadOnly 줄은 값이 같아 무동작이다, 10.1절). 다만 재입력한 대입은 값이 같아도 override 경로로 기록되므로(10.2절), 출력 재입력이 A5 뒤 기본값 전파를 끊는 문제는 A5가 기준 차이 출력과 함께 정한다(20절 열린 문제). struct 원소 컨테이너는 위 표의 `a=`·`a[i]={}`·`m[k]={}`·멤버 줄로 펼친다. `--json`은 ExportJson, `--all`은 Report 용도다.
- 출력은 용도의 제외 플래그를 뺀 모든 필드다. 기준 차이 출력은 A5 레코드가 생긴 뒤 옵션으로 더한다(그때 Atomic struct는 통째, 아닌 struct는 멤버 단위).

### 10.5 JSON API

- `ExportJson(대상, JsonWriter&, options)`·`ImportJson(JsonValue, AssignmentTarget, options, diagnostics) -> Result<PreparedAssignments>`. import도 10.2절 묶음을 거친다(원자성·override 경로가 같다). 새 값(설정 struct·staged 객체)으로 읽는 경우는 실패하면 그 값을 버린다.

## 11. InstancedStruct(B9j)

- C++ 값 타입 `InstancedStruct<Base = void>`(ObjectRuntime `Object/InstancedStruct.h`): struct 타입 lease(값이 사는 동안 타입 등록을 붙잡는다), 할당한 값 블록, allocator. 이동 전용, `TryClone`. API: `Emplace(TypeLease, Allocator&)`(기본 생성), `Emplace<S>(Allocator&)`, `Reset`, `Type()`, `Get<S>()`(native 키 대조), `IsEmpty`.
- B9j는 `InstancedStruct<>`(제한 없음)를 연다. `InstancedStruct<Base>`는 12절(B9k).
- **담을 수 있는 struct**(쓰기·`Emplace`·decode가 등록 형질로 검사, `Emplace<S>`는 static_assert):
  - 강한 참조가 없다(`ContainsStrongReference` false). 값 블록이 소유 객체 밖이라 참조 쓰기 범위 검사를 통과할 수 없다(Array<S>와 같은 이유 — RF 4.3절).
  - Registry 컨테이너(ObjectRefArray·StructRefArray·ObjectRefMap)를 담지 않는다.
  - InstancedStruct를 (직간접으로) 담지 않는다(새 등록 형질 `ContainsInstancedStruct`).
  - Required 멤버가 없다(`ContainsRequired` false). 2c는 등록 형질로 검사할 최상위 필드를 고르는데 담긴 타입은 실행 중에 정해지기 때문이다(OF 4절 "구현에서 정한 점").
- **깊이**: InstancedStruct가 놓인 위치 깊이 p(그 필드를 감싼 struct 단계 수, 최상위 필드면 0)와 담긴 struct의 `Depth`(등록 값)의 합이 `MaxStructDepth`(16) 이하여야 한다. 쓰기·`SetInstancedType`·decode 1단계·encode가 검사하고, encode는 Fatal 대신 InvalidArgument로 끝낸다. 위의 "InstancedStruct를 담지 않는다" 규칙과 합쳐 codec 위치 프레임 1 + 16(MS/TypeSchema.h:61-62)과 `MaxPropertyPathSteps` 17, 진단 경로 버퍼(`BuildPath` 33단계)를 넘지 않는다. C++가 `Emplace`로 직접 넣은 깊이 위반은 encode에서 드러난다. RMC2303·RF 4.1·4.6절 깊이 규칙에 이 합을 더한다(21절).
- 위치: 클래스 필드·struct 멤버·Array·StaticArray 원소·Map 값(원소·값 자리 wire는 u64 길이 + 내용 — 1.2절).
- schema: `value = {InstancedStruct, Base ID 또는 0}`.
- wire InstancedStruct(17). struct 버전 마이그레이션은 중첩 struct와 같다(RF 4.4절).
- decode 1단계: Record schema로 struct 타입을 연다(`AcquireValueType`). 미등록은 새 정책 `unknownStructType`: Reject(기본) 또는 ReportAndClear(`UnknownStructType` 진단 뒤 비움). 담을 수 없는 struct·깊이 위반은 InvalidArgument. 2단계는 `ObjectAllocator()`로 값 블록을 만든다.
- GC 없음. Required 거부(RMC2202).
- 경로: `f` 연산 `SetInstancedType(TypeId)`(그 타입 기본값으로 교체)·`Clear`, 멤버 `f.member`는 지금 타입으로 해석한다. 문자열 경로 해석이 값의 타입에 의존하므로 `ParsePropertyPath`에 객체 문맥 판(registry, handle)을 더한다. `PathStep`은 그대로다(멤버 필드 ID가 전역 유일 Guid).
- 차이 단위(A5): 통째. 같은 타입이면 기준 대비 멤버 차이로 쓰는 UE 방식(18.11절)은 writer 선택이라 형식을 바꾸지 않고 뒤에 더할 수 있다.
- 소비자: Sparks 데이터 테이블 행 저장 `OrderedMap<Name, InstancedStruct<>>`(SL 1090행 `@tableRow`), CU6·데이터 에셋.

## 12. struct 상속(B9k)

- `ROCKS_STRUCT() struct Derived : public Base { … }`. 리플렉션 base는 하나, public 비가상이다. 여러 단 상속을 허용한다.
- **필드 없는 비리플렉션 base**: 비정적 데이터 멤버가 (재귀적으로) 없고 virtual 함수·virtual base가 없는 C++ base(CRTP 도우미 등)는 개수와 무관하게 허용한다. UE는 USTRUCT가 아닌 base를 `#if CPP`로 UHT에서 숨겨 허용한다(UhtScriptStruct.cs:1100, PerPlatformProperties.h:225-229의 `TPerPlatformProperty`). Rocks MetaCompiler는 Clang AST로 모든 base를 보므로 숨기는 방법이 없고, 대신 "필드 없음"을 확인해 허용한다. 필드가 있는 비리플렉션 base는 RMC2301(그 필드가 리플렉션되지 않는다).
- Binds 미러는 base가 될 수 없고 base를 가질 수도 없다(RMC2301). 리플렉션 struct는 virtual 함수를 가질 수 없다(RMC2301 — 다형성은 InstancedStruct로).
- schema: struct의 `baseType` = Base ID. `fields`는 base 필드 + 자기 필드(펼침, owner 표시 — 클래스와 같다, RF 1.4절). 필드 ID는 base 소유 그대로다.
- Record: schema = 파생 ID, version = 파생 버전, base 필드 TLV가 그대로 들어간다(펼침 — RF 결정 4, UE와 같다). base 배치를 바꾸면 파생도 버전을 올린다(RF 2.2절 클래스 규칙, MetaCompiler는 강제하지 않는다).
- 형질: `ContainsStrongReference`·`RequiresWorldOnlyOwner`·`ContainsRequired`·`ContainsInstancedStruct`·`Depth`는 상속 필드까지 계산한다. Atomic은 base에서 상속되고 끌 수 없다.
- 생성 멤버 thunk: 지금 struct 멤버 thunk는 멤버 포인터의 소유 타입으로 `void*`를 캐스트한다(GeneratedFieldCodec.h:1714-1743, GeneratedPathAccess.h:501-509). base 멤버면 파생 객체를 base로 잘못 캐스트할 수 있으므로(base 하위 객체가 offset 0이 아닐 때) struct 멤버 thunk도 소유 struct를 명시 인자로 받게 바꾼다(`EncodeStructMember<Derived, &Base::m>` 꼴 — 클래스 필드 thunk는 이미 소유자를 따로 받는다, GeneratedFieldAccess.h:317).
- 값 의미: 필드 타입이 Base면 정확히 Base 값이다. 다형성은 `InstancedStruct<Base>`로 한다.
- 도구 이름은 상속 포함 대소문자 무시 유일(RMC2203 — 클래스와 같다).
- `InstancedStruct<Base>`: `value.type` = Base ID. 담긴 타입이 Base 또는 파생이어야 한다(쓰기·decode 1단계, TypeRegistry의 struct 파생 조회 `IsStructA`를 새로 둔다). C++ `Emplace<S>`는 `static_assert(__is_base_of(Base, S))`로 막는다(형질 내장은 ObjectRuntime·생성 코드가 이미 쓴다 — RuntimeTypeOps.h:406·457). Base가 InstancedStruct를 담으면 RMC2308.

## 13. Text(B9l)

- C++ `rocks::Text`(Core/Source/Runtime/Localization/Public/Localization/Text.h:28-59 — 불변 DAG, 이동 전용). 기본 생성은 비어 있음. 클래스 필드·struct 멤버·Array·StaticArray 원소·Map 값.
- ObjectRuntime이 Localization에 의존한다(Localization은 Core만 의존 — Localization.Build.cs. ObjectRuntime 의존 MetaSchema·Task·Serialization과 순환이 없다).
- Localization 추가:
  - `Text::TryGetPersistentForm(TextPersistentView&) -> Result<void>`: 비어 있음·Invariant·Localized면 형태와 뷰, Format·Integer면 Unsupported. 지금 Text는 형태를 조회하는 공개 API가 없다.
  - `Text::DeepClone(Allocator&) -> Result<Text>`: 대상 allocator로 노드를 새로 만든다. 지금 `Clone`은 노드를 공유하고 "각 allocator는 마지막 노드 반환까지 살아 있어야 한다"(Text.h:25).
- **allocator 수명**: decode는 `ObjectAllocator()`로 만든다. FieldValue로 읽어 내보낼 때와 Registry 사이 값 복사(에셋 Registry → World Registry)는 `DeepClone`으로 대상 allocator에 새로 만든다. 그래서 한 Registry의 allocator 노드를 다른 수명의 소유자가 쥐지 않는다.
- wire Text(18)(1.2절). Localized는 `TextId`(Guid — Catalog.h:10)·`sourceRevision`(u64)·fallback 원문.
- 한도: 원문은 `TextCreateLimits.maxSourceBytes`(1 MiB)와 `maxValueBytes`.
- encode: Format·Integer 이력은 Unsupported(경로와 함께 오류). 런타임에 만든 조합 문구는 저장 대상이 아니다.
- 같음: wire byte. GC 없음. Required 거부.
- 경로: 단일 값(`FieldValue.text`, 읽기는 DeepClone).
- 영속 범위: 잎 형태 셋(비어 있음·Invariant·Localized)과 형태 byte만 둔다. 새 형태(문자열 표 등)는 형태 값 하나와 codec 함수 하나로 더한다. 형식 지식은 Localization의 `TryGetPersistentForm`·생성 함수와 codec 한 곳에만 있다.
- TextId의 정체성(복제 때 유지·재발급, 누가 발급하는가)은 지역화 설계와 함께 정한다(20절 열린 문제).

## 14. bitfield bool(B9m)

- **받는 형식**: `uint8 b : 1`·`uint16 b : 1`·`uint32 b : 1`·`uint64 b : 1`(폭 1, 부호 없는 정수)을 `ValueKind::Bool` PROPERTY로 받는다. **`bool b : 1`은 거부한다**(UE와 같다 — 사용자 결정 U5, UhtBoolProperty.cs:340-343). 폭이 1이 아니거나 부호 있는 정수·enum이면 RMC2207(UE도 폭 1만 — UhtPropertyParser.cs:1047-1053). UE UHT도 uint8·16·32·64 폭 1 bitfield를 모두 uint8 크기 bool로 받는다(UhtByteProperty.cs:252-262, UhtUInt32Property.cs:72-78, UhtBoolProperty.cs:31-44).
- **접근**: 멤버 포인터 대신 생성 접근자. `GeneratedType<Owner>`(`ROCKS_GENERATED_BODY`가 friend로 둔다 — MS/Annotations.h:87-89)에 `static bool Get_<name>(const Owner&) noexcept`·`static void Set_<name>(Owner&, bool) noexcept`를 낸다. read·write·writeStaged·encode·decode·경로·struct 멤버 thunk에 접근자 판(`detail::BitfieldFieldOps<Get, Set>`)을 둔다. 지금 thunk는 모두 `&::Owner::member` 멤버 포인터 기반이다(MC/CppEmitter.cs:271, GeneratedFieldCodec.h:1714).
- ByteOffset·ByteMask는 두지 않는다(RF 원칙 "raw offset 공개 없음"). 비트 배치는 컴파일러가 처리한다. UE는 0으로 채운 버퍼에 SetBit을 불러 바이트를 찾는다(PropertyBool.cpp:98-117).
- descriptor 필드 `"bitfield": true`: 의존 모듈이 상속 필드 thunk를 만들 때 base의 접근자를 부르기 위해서다. 런타임 `FieldSchema`는 바뀌지 않는다.
- wire Bool, 경로·텍스트·차이 저장은 bool과 같다.
- NATIVE bitfield: 지금은 NATIVE·PROPERTY 모두 bitfield를 거부한다(MC/ClangAstParser.cs:683-686·710-713). NATIVE bitfield는 정수·`bool` 모두 허용하고(강한 참조를 담을 수 없다), 탐침은 쓰지 않는다(정수·bool 형식인지만 AST로 본다). RMC2207은 PROPERTY와 Binds 미러에만 건다(UHT도 UPROPERTY만 본다).
- Binds 미러 안 bitfield는 RMC2207이다(`VerifyBinds`가 `decltype(Real::m)`·`&Real::m`·구조적 바인딩을 쓴다 — CppEmitter.cs:434-461. Core 값 타입에는 bitfield가 없다).

## 15. 진단 코드

| 코드 | 판정 | 뜻 |
|---|---|---|
| RMC2202(확장) | MetaCompiler | 지정자-종류 불일치에 새 경우를 더한다: Required는 ClassRef와 그 컨테이너·참조 StaticArray·ObjectRefMap 값·Soft·ClassRef 원소 Set까지(WeakRef·Name·Optional·InstancedStruct·Text는 거부), MaxElements는 Map·Set까지(StaticArray는 거부), Required + DuplicateTransient·NonPIEDuplicateTransient 조합 거부. 타입 지정자를 다른 매크로에 쓰면 매크로별 지정자 표에 없으므로 지금의 "모르는 지정자" 오류다 |
| RMC2203(확장) | MetaCompiler | 타입 도구 이름의 형식·대소문자 무시 중복(모듈과 import 범위), `none` 예약 |
| RMC2207(새) | MetaCompiler | PROPERTY·Binds 미러의 bitfield 규칙 위반(NATIVE bitfield는 정수·bool 모두 허용): 폭 ≠ 1, `bool : 1`, 부호 있는 정수·enum·실수 bitfield, Binds 미러 안 bitfield |
| RMC2208(새) | MetaCompiler | `Within` 오류: 대상이 리플렉션 클래스가 아님, 부모 Within의 파생이 아님, 철자를 풀 수 없음 |
| RMC2209(새) | MetaCompiler | Deprecated가 아닌 타입의 필드가 Deprecated 클래스를 가리킴 |
| RMC2301(뜻 변경) | MetaCompiler | struct 상속 규칙 위반: 리플렉션 base 둘 이상, 필드 있는 비리플렉션 base, virtual·non-public base, virtual 함수, Binds 미러가 base이거나 base를 가짐 |
| RMC2303(확장) | MetaCompiler | 깊이 초과에 InstancedStruct<Base>의 위치 깊이 + Base 깊이를 더한다 |
| RMC2304(확장) | MetaCompiler | 배열 원소로 쓸 수 없는 형식에 Optional·Map·Set·StaticArray를 더한다 |
| RMC2305(새) | MetaCompiler | Map·Set 오류: 키·원소 종류, 직접 중첩, `HashMap`·`HashSet` PROPERTY |
| RMC2306(새) | MetaCompiler | Optional 내부 형식 오류(중첩 Optional 포함) |
| RMC2307(새) | MetaCompiler | 정적 배열 오류: 다차원, N 범위(1..65,535), 원소 형식 |
| RMC2308(새) | MetaCompiler | InstancedStruct 오류: Base가 리플렉션 struct가 아님, Base가 담을 수 없는 struct(11절 조건) |
| RMC2401(확장) | MetaCompiler | enum underlying 8종을 받는다. uint64 열거자 값이 int64 최댓값을 넘음 |
| RMC2003(재사용) | MetaCompiler | WeakRef·ClassRef 대상이 리플렉션 클래스가 아님(지금 참조 대상 진단 — MC/SchemaValidator.cs:273) |

한 사례에는 코드 하나다. 컨테이너 쪽 위치 규칙이 먼저다: Array 원소 위치에 쓸 수 없는 형식은 RMC2304, Map·Set의 키·값·원소 위치는 RMC2305, StaticArray 원소는 RMC2307이고, RMC2306은 Optional의 내부 형식에만 쓴다(예: `Array<Optional<T>>`는 RMC2304, `OrderedMap<K, Optional<V>>`는 RMC2305, `Array<OrderedMap<K,V>>`는 RMC2304, `Optional<Optional<T>>`는 RMC2306).

런타임 진단(`FieldDiagnosticKind`): `UnknownClassReference`·`UnknownStructType`·`TransientType`·`DeprecatedType`을 더한다. `ExternalReference`의 값: 0 = 거부, 1 = 외부 null(Admit의 Null), 2 = weak 집합 밖 null(Admit의 Null·Import, Reject 정책, Include 최종 집합 밖), 3 = Transient 클래스 대상 null, 4 = Deprecated 클래스 대상 null(3·4는 저장 정책 Exclude).

## 16. 커밋 분할과 검증

메인 트리 구현 에이전트는 한 번에 하나다. 커밋마다 Debug·format·바뀐 영역 시험을 돌리고, Asan 등 전체 체인은 묶음 끝에 돈다(B9a1 직후, B9a–B9e 끝, B9f–B9h 끝, B9i 끝, B9j–B9m 끝 — B9a1은 생성 코드·TypeRegistry 전반의 동작 불변 리팩터라 직후에 한 번 더 돈다). 각 하위 단계는 구현 뒤 다수 서브에이전트 교차검증을 거친다. 규모: S < 800행, M 800–2,000행, L 2,000–4,000행(생성 코드·시험 포함 추정).

| 커밋 | 범위 | 선행 | 규모 |
|---|---|---|---|
| B9a1 | 종류 성질 표(MS constexpr·C# 표·ModuleDescriptor 검증), 대조 헤더 생성과 ToolTests 컴파일, TypeRegistry 범위 검사 제거 — 동작 불변 | A0-O 완료 | M |
| B9a2 | Int8·Int16, enum underlying 8종 | B9a1 | M |
| B9a3 | Serialization wire 14–18 reader·writer, ValueArray 원소 tag 확장, Record·ValueArray 시험 | B9a1 | M |
| B9b1 | Core Transform3f·Transform3d(API·변환·시험) | — | M |
| B9b2 | Atomic 지정자·TypeFlags·RegisterStruct mask·descriptor·`IsAtomic`, 미러 17종(Transform3f·3d 추가, 16종 Atomic, Transform 멤버 SaveGame은 B9h1 뒤) | B9a1, B9b1 | S |
| B9c1 | Core `NamePool::TryFind`·`NameTextEquals`·base·번호 분해 | — | S |
| B9c2 | Name 종류·`RegistryOptions.names`·1단계 수집·2a intern·작업량 청구·관찰 struct 제한·FieldValue·경로·Array 원소 | B9a3, B9c1 | L |
| B9d1 | WeakRef PROPERTY·Array 원소·문맥 weak 참조·정책별 처리(Include 지연 덮어쓰기)·hook `weak` | B9a1, A0-Oe | L |
| B9d2 | ClassRef·타입 도구 이름(TypeSchema·지정자·TypeRegistry 색인)·`unknownClass` 정책·Array 원소 | B9a1 | M |
| B9e | Optional·`ObjectAllocator()` | B9a3(B9c2) | M |
| B9f1 | Core OrderedMap·OrderedSet | — | M |
| B9f2 | Map·Set 종류·codec·`FieldSchema.key`·키 대상 칸·진단 경로 frame 일반화 | B9f1, B9c2, B9d1, B9d2 | L |
| B9f3 | 경로 key 단계(`PathSelector`·`PathKey`)·Map·Set 경로 연산 | B9f2 | M |
| B9f4 | ObjectRefMap(Registry API·staged·Trace·관찰자) | B9f3 | L |
| B9g | 정적 배열·NATIVE 형질 배열 특수화·성질 표 대조 시험 | B9a3(원소 종류는 c·d 뒤) | M |
| B9h1 | 필드 플래그 4종·`includeFlags`(멤버마다)·Required 조합 거부·Transform 미러 멤버 SaveGame | B9a1, B9b2 | S |
| B9h2 | 클래스 Transient·Deprecated(저장 정책 `unsaveable`·로드 진단·RMC2209)·Within(`OuterSummary`·정체성 규칙)·ClassRef 쓰기의 Deprecated 거부와 decode 진단 | B9a1, B9d1, B9d2, A0-Oe | L |
| B9i1 | Core `JsonWriter::Float`·`JsonValue::Position`·StringSplit escape 옵션 | — | S |
| B9i2 | B6 확장: 객체 변경 표지·묶음 게시 구간(대상 재확인·관찰자 묶음 예약)·준비 단위 셋·대상 고정(ObjectRuntime 시험으로 원자성 고정) | B9f4 | M |
| B9i3 | ReflectionText 모듈·용도·명령행 표기·대입 묶음 API·resolver·진단 | B9a–B9h, B9i1, B9i2 | L |
| B9i4 | JSON export·import(객체·staged·struct 값, mapMerge) | B9i3 | L |
| B9j | InstancedStruct<>·담기 조건·깊이 합·객체 문맥 경로 해석 | B9i4 | L |
| B9k | struct 상속·비리플렉션 빈 base·struct 멤버 thunk 소유자 인자·`InstancedStruct<Base>` | B9j | L |
| B9l | Text·Localization `TryGetPersistentForm`·`DeepClone` | B9i4 | M |
| B9m | bitfield bool(접근자 판 일곱 갈래) | B9a1 | L |

Core 커밋(B9b1·B9c1·B9f1·B9i1)은 다른 단계와 독립이라 먼저 해도 된다(메인 트리 구현은 순차). 종류 단계는 B9i3 뒤라면 그 종류의 텍스트 표기를 같은 커밋에서, 앞이라면 B9i3·B9i4에서 넣는다.

## 17. 시험 목록

**B9a**: 성질 표 단위 시험(모든 종류 × 성질이 1.3절 표와 같음), 대조 헤더 양방향(C# 표를 바꿔 넣은 사례가 컴파일 실패), int8·int16 그래프 왕복·범위 밖 I64 거부, enum 8종 왕복, uint64 값 상한 RMC2401, 평범한 `char` 거부, 경로 쓰기 범위 검사, wire 14–18 reader의 잘림·count 사전 검사·원소 tag 거부·present byte 범위.

**B9b**: Transform3f·3d — 점·벡터·방향 변환, 역변환, `ToMatrix`·`TryFromMatrix` 왕복, shear Unsupported·0 scale InvalidArgument, 변환 함수(넓힘 정확·좁힘 반올림). Atomic — STRUCT에만(CLASS는 모르는 지정자), descriptor 왕복, RegisterStruct mask(클래스 비트 거부), 미러 16종 Atomic·Transform 비Atomic 등록, `IsAtomic`. A5 시험 목록에 넘길 것: Atomic 차이 쓰기·override 단위·Transform 멤버 단위.

**B9c**: Core `TryFind`(없으면 NotFound·할당 없음·표시 변형은 비교 토큰)·`NameTextEquals`(번호 접미·대소문자), 그래프 왕복(번호 접미·None·대소문자 표시 보존), pool 없는 Registry의 Unsupported, 다른 pool 토큰 WrongContext, NFC·UTF-8·3,072 byte·NUL 거부, 2a intern의 CapacityExceeded(생성자 미실행·Registry 불변·이미 들어간 entry는 남음), 1단계 청구가 2a 몫을 덮음(작업량 한도 fixture), 관찰 struct의 Name 거부, Array<Name> 왕복, 경로 쓰기.

**B9d**: WeakRef 왕복(집합 안), 정책별 표(Reject는 null·진단·성공, Admit의 Null·Import → null·진단(값 2)과 Reject, 강한 참조가 먼저 Import한 대상을 weak가 slot 캐시 없이 다시 물음, Include 지연 덮어쓰기 — 노드 순서를 바꿔도 같은 결과·지연 진단의 경로·objectIndex·메모리 청구), 죽은 대상 encode null, 외부 번호 decode 거부, 기대 타입 위반 decode 거부, Array<WeakRef> 왕복, GC가 weak만 남은 대상을 회수, 도달 검사에서 weak 제외. ClassRef 왕복, 미등록(Reject·ReportAndNull, 키 위치는 늘 거부), Base 위반, struct ID 넣기 거부, 추상 클래스 허용, Required null 저장 거부, (B9h2 뒤) Deprecated 클래스 ClassRef 로드 진단과 재저장 성공, 타입 도구 이름(기본·`Name=`·충돌 RMC2203·`none` 예약)·TypeId 32자·36자 입력.

**B9e**: set·unset·누락 세 경우 왕복(누락 → 생성자 값), Optional<String>·Optional<값 struct> 왕복과 2단계 할당 실패 rollback, 거부 내부 형식 RMC2306·원소 위치는 컨테이너 쪽 코드(RMC2304·2305), 순환 RMC2302, 경로 Emplace·Reset·unset 멤버 NotFound, FieldValue kind = 내부 종류.

**B9f**: Core — 순서, 같은 키 대입 위치 유지, 제거 뒤 재삽입, 압축, Reserve, 할당 실패 무변경, 해시 충돌 mask. 리플렉션 — 키 종류별 왕복(Enum 키의 레코드 의존·native 대조), 순서 보존 왕복, 중복 키 decode 거부(String·Name 대소문자), 키 위치의 미선언 enum은 정책과 무관하게 거부, 잘린 payload·count 사전 검사, MaxElements 저장 거부·로드 허용, Name 키 2a intern, 값 struct 누락 멤버 = 원소 기본값, WeakRef 값 왕복, 진단 경로가 삽입 순번, 거부 형식 RMC2305. 경로 — key 읽기·쓰기·Erase·옛 값, `InsertDefault`, 없는 Name 키 NotFound 할당 없음, `SamePropertyPath` key 비교, key 따옴표 문법. ObjectRefMap — Marking 중 staged 맵, Commit 간선 검증, 관찰자 계수, 장벽·shade, null 값, Required 값, 실패 시 불변.

**B9g**: 원소 종류별 정적 배열 왕복, ObjectRef 정적 배열의 GC·SetReference·순환 그래프, 참조 struct 정적 배열, NATIVE 형질의 배열 강한 참조 검출, count 불일치 InvalidArgument, 다차원·N 범위 RMC2307, 경로 삽입 Unsupported, Required 원소, 성질 표 대조(모든 ○ 칸이 실제로 등록·왕복됨 — 뒤 단계마다 행 추가).

**B9h**: 필드 플래그 지정자, 복제 정책(에셋·PIE)별 저장 집합, includeFlags 저장·로드 대칭(멤버마다, Atomic 통째, Transform 멤버), 2c의 includeFlags 밖 필드·멤버 제외, Required + DuplicateTransient RMC2202. 저장 정책 Exclude(저장 집합의 저장 불가 객체 거부, 참조 null + 진단 값 3·4, Required 위치 규칙, Include 제외, root 거부)와 Keep(스냅샷 왕복에 Transient·Deprecated 객체와 그 참조 보존), 부분 트리 저장 불가 판정, Transient·Deprecated 노드의 로드 진단과 Exclude 재저장에서 빠짐(클래스를 나중에 Transient로 바꾼 패키지를 열고 고침), RMC2209, ClassRef 쓰기의 Deprecated 거부, Within 생성 성공·실패·상속·RMC2208, 그래프 안 Outer의 1단계 Within, 기준 Outer의 2a Within, Outer 없는 객체.

**B9i**: 종류별 명령행 Format·Parse 왕복(경계값·escape 표·Flags `+`·비유한 실수·Map 첫 `:`), JSON 왕복(모든 종류, Map 순서, float 단정도 표기), 용도별 제외(Edit·Copy·PieCopy·Report의 출력·입력 — Edit 입력의 NonPIEDuplicateTransient·TextExportTransient 허용), 엄격성(모르는 키·대소문자 중복 키·입력 제외 필드·숨김 열거자·ReadOnly), 값이 같은 ReadOnly 대입 무동작(override 경로에 없음), 의미 오류의 JSON 위치, 오류를 모두 모아 보고, resolver 없음·있음(Hard·Weak·Soft 구분, Soft는 로드하지 않음, Append에서도 호출·진단), 대입 묶음(같은 경로 두 번 오류, 앞 대입 위 해석, `Touches`·`Append`, override 경로 정규화 — Atomic·컨테이너, 값 변화 없는 대입 포함, key 안 `=`의 대입 분해), 준비 단위(값·ObjectRef·ObjectRefArray 멤버가 섞인 인라인 struct의 단위별 게시와 장벽·관찰자 기록), Publish 원자성(변경 표지 Busy, **Prepare 뒤 대상 Discard·GC → Publish가 StaleHandle로 실패하고 객체 불변**, 대상 고정이 GC 회수를 막음, 관찰자 예약 실패 주입 시 객체 불변, 할당 실패 주입), staged 객체 대상의 ReadOnly 허용, JSON import 의미(struct 병합·컨테이너 교체·mapMerge), asset-info 출력 → asset-set 왕복, A2 표기 시나리오(AS 13절 예 `SourceColorSettings.ColorSpace=sRGB`).

**B9j**: 왕복(비어 있음·값), 원소·Map 값 자리 wire, 마이그레이션, 미등록 타입 정책 2종, 담을 수 없는 struct(참조·Registry 컨테이너·InstancedStruct·Required) 거부, `Emplace`(등록 형질)·`Emplace<S>`(static_assert)가 InstancedStruct를 담은 struct를 거부, 깊이 합 초과가 쓰기·decode에서 InvalidArgument이고 깊은 위치에 C++가 직접 Emplace한 깊이 합 위반은 encode에서 Fatal 없이 InvalidArgument, Map 값 InstancedStruct 왕복, 타입 lease로 Unregister Busy, 경로 형 바꾸기·멤버 쓰기·객체 문맥 경로 해석, 텍스트·JSON `_structType`.

**B9k**: 상속 struct 왕복(base 필드 펼침, 필드 ID), 여러 단 상속, 필드 없는 비리플렉션 base 허용(base 하위 객체 offset이 0이 아닌 배치 포함), 도구 이름 충돌 RMC2203, RMC2301 사례(base 둘·virtual·필드 있는 비리플렉션 base·virtual 함수·Binds), 형질 상속(참조·Required·Atomic·깊이), 경로 쓰기 base 멤버, `InstancedStruct<Base>`의 파생 허용·무관 struct 거부(쓰기·decode), RMC2308.

**B9l**: 세 형태 왕복, Format·Integer encode Unsupported, 원문 한도, 2단계 할당 실패 rollback, FieldValue 읽기·Registry 사이 복사의 DeepClone(원래 allocator 해제 뒤에도 유효), Array<Text>·Map 값 Text, 경로 쓰기, JSON 문자열·객체·null.

**B9m**: 형식 4종 왕복, `bool : 1` RMC2207, 이웃 비트 보존(같은 바이트의 다른 bitfield·일반 멤버), struct 멤버 bitfield·Array<S> 원소, 다른 모듈 파생의 상속 bitfield(descriptor `bitfield`), 경로 쓰기, RMC2207(폭 3, `int8 : 1`, Binds 안), NATIVE bitfield 허용.

## 18. UE 5.8 대조

### 18.0 UE 파일 위치

| 파일 | 위치 |
|---|---|
| `PropertyX.cpp`(PropertyMap·PropertySet·PropertyBool·PropertyArray·PropertyStruct·PropertyName·PropertyClass·PropertyBaseObject·PropertyWeakObjectPtr·PropertyOptional·PropertyTag·EnumProperty·TextProperty), LinkerSave.cpp, LinkerLoad.cpp, Obj.cpp, GarbageCollection.cpp | CUO/Private/UObject/ |
| PackageHarvester.cpp, SaveContext.cpp | CUO/Private/UObject/SavePackage/ |
| ArchiveUObject.cpp, DuplicateDataWriter.cpp | CUO/Private/Serialization/ |
| PropertyOptional.h, PerPlatformProperties.h, PerPlatformPropertiesImpl.inl, UObjectGlobals.h, LinkerLoad.h, PropertyPathName.h, OverriddenPropertySet.h | CUO/Public/UObject/ |
| InstancedStruct.h / InstancedStruct.cpp | CUO/Public/StructUtils/ / CUO/Private/StructUtils/ |
| SubScriptStructOf.h | CUO/Public/Templates/ |
| Map.h, Set.h, SparseArray.h | Engine/Source/Runtime/Core/Public/Containers/ |
| Archive.h | Engine/Source/Runtime/Core/Public/Serialization/ |
| MathFwd.h | Engine/Source/Runtime/Core/Public/Math/ |
| NameTypes.h / UnrealNames.cpp | Engine/Source/Runtime/Core/Public/UObject/ / Engine/Source/Runtime/Core/Private/UObject/ |
| Text.cpp, TextHistory.cpp, TextHistory.h | Engine/Source/Runtime/Core/Private/Internationalization/ |
| JsonObjectConverter.cpp | Engine/Source/Runtime/JsonUtilities/Private/ |
| UhtMapProperty·UhtSetProperty·UhtArrayProperty·UhtOptionalProperty·UhtBoolProperty·UhtByteProperty·UhtUInt32Property·UhtStructProperty·UhtEnumProperty·UhtObjectPropertyBase(.cs) | UHT/Types/Properties/ |
| UhtProperty.cs, UhtScriptStruct.cs, UhtClass.cs, UhtEnum.cs | UHT/Types/ |
| UhtPropertyParser.cs | UHT/Parsers/ |
| UhtScriptStructSpecifiers.cs | UHT/Specifiers/ |

### 18.1 정수 폭

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| int8·int16 | FInt8Property UT:2267, FInt16Property UT:2301 | ValueKind Int8·Int16 | 같다 |
| enum underlying | 9개 토큰(Int 포함, UhtEnum.cs:41-93), 기본 Int32(UhtEnumProperty.cs:59) | 8종, 명시 필수 | underlying 명시(RF 3.4절) |
| 저장 폭 | native 폭 | I64 | RF 결정 14(폭 확장의 wire 호환) |

### 18.2 Atomic·수학 타입

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 플래그 | `STRUCT_Atomic` "serialized as a single unit", `STRUCT_Immutable` 이진 직렬화(CH:1056-1060). `immutable`은 Atomic도 켠다(UhtScriptStructSpecifiers.cs:30·38). Immutable은 NoExportTypes 밖에서 오류(UhtScriptStruct.cs:1089-1094) | Atomic만 | Rocks는 이진 배치가 없다(RF 결정 10) |
| 태그 저장의 Atomic | 멤버마다 `Identical`로 쓸지 정하고(CC:2016-2018), 쓰는 멤버만 기준 없이 깊게 쓴다(CC:2026-2029). 1984행 주석과 달리 같은 멤버는 건너뛴다 | 모든 멤버(통째) | UE에서 "통째"는 native Serializer·Immutable 경로가 만든다. Rocks Atomic은 그 결과를 따른다(3.1절) |
| 수학 타입 | native Serializer(PR:53-68 FVector, PR:307-334 FLinearColor·FColor, PR:337-356 FQuat) 또는 Immutable 이진(CC:3338-3343·3404-3421). FTransform3f·3d는 serializer가 꺼져 있고(PR:485-495) immutable(NET:2076·2096) | 미러 16종 Atomic | 같은 결과(통째) |
| FTransform | `USTRUCT(noexport, …)` — immutable·atomic 아님(NET:2116), C++ 타입은 `TTransform<double>`(MathFwd.h:53), 멤버 Rotation·Translation·Scale3D가 각각 immutable이고 SaveGame(NET:2120·2124·2128) | `Transform`(혼합) 비Atomic, 멤버 SaveGame | 같다. Rocks Transform은 이동 double·회전·크기 float라 UE FTransform(전부 double)과 정밀도가 다르다(EIP 2.3절) |
| 비교 | `CompareScriptStruct`는 Atomic과 무관한 멤버별 비교(CC:3663-3694, PropertyStruct.cpp:139-142) | Record 전체 byte | RCM 63행 |
| 텍스트 | Atomic이면 모든 멤버 출력(CC:3548-3553) | 같다 | — |
| 상속 | `STRUCT_Inherit` = HasInstancedReference·Atomic(CH:1131, UhtScriptStruct.cs:980) | Atomic 상속 | 같다 |

### 18.3 Name

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 비교 | "case-insensitive, but case-preserving"(NameTypes.h:629), `==`는 ComparisonIndex + Number(NameTypes.h:808-811), ComparisonIndex 공유는 Strnicmp(UnrealNames.cpp:220-242) — ASCII만 접는다 | ASCII만 대소문자 무시 | 같다 |
| 저장 | 패키지 name map 인덱스(Display) + Number(LinkerSave.cpp:316-358, LinkerLoad.h:1150-1161) | Record 안 표시 문자열 | Record는 자기 완결 TLV. 패키지 name 표는 A1 후보 |
| 길이 | NAME_SIZE 1024 UTF-16 단위(null 포함, NameTypes.h:57), 초과는 checkf 뒤 대체 이름(UnrealNames.cpp:3271-3285) | UTF-8 3,072 byte, 초과는 InvalidArgument | UE 이름이 모두 들어가는 byte 상한, 예외 대신 Result |
| 텍스트 | Delimited가 아니면 그대로(None은 `None`), Delimited면 None은 `""`(PropertyName.cpp:42-53) | 출력 그대로·None은 `None`, 입력은 빈 값·`None` | 같다(비Delimited) |
| 차이 판정 | 대소문자 무시 `Identical` | 표시 byte | RCM 63행(사용자 결정 U7) |

### 18.4 WeakRef·ClassRef

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| weak 저장 | `Ar << Object`(ArchiveUObject.cpp:133-155). harvester가 수집하지 않으면 null(LinkerSave.cpp:240-250). 쿠킹이거나 같은 outermost의 다른 패키지(external actor)면 강한 참조처럼 import로 수집한다(PackageHarvester.cpp:1102-1122) | 같은 그래프 안만 보존, 밖은 null + 진단 | 편집기 저장은 같다. 쿠킹·외부 객체 패키지의 weak import는 World 외부 객체 패키지를 만들 때 정한다(20절 열린 문제) |
| weak GC | 참조 정보 없음(GarbageCollection.cpp:6833-6835, UT:3116-3121) | 방문 없음 | 같다 |
| weak 로드 검사 | `CheckValidObject` 불일치는 경고 후 null(PropertyWeakObjectPtr.cpp:98, PropertyBaseObject.cpp:913·953-983) | InvalidArgument | 강한 참조와 같은 규칙(잘못된 값은 로드에서 드러낸다) |
| weak NonNullable | 불가(PropertyWeakObjectPtr.cpp:56) | Required 거부 | 같다 |
| TSubclassOf 저장 | UClass 객체 참조(FClassProperty는 FObjectProperty 파생) | TypeId | 영속 ID와 실행 핸들 구분(RCM 30행) |
| MetaClass 검사 | ImportText만(PropertyClass.cpp:125-140). 로드 때 검사 없음 | 쓰기·encode·decode | 잘못된 값은 로드에서 드러낸다 |
| 타입 이름 | 클래스 경로(`/Script/Module.Class`) | 타입 도구 이름 | AS 13절의 한정하지 않은 `<Type>`과 같게 |

### 18.5 Optional

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 레이아웃 | intrusive unset 또는 값 + bool(PropertyOptional.h:28-34·139-177) | Core Optional(값 + bool) | 메모리 배치를 리플렉션에 공개하지 않는다 |
| 저장 | `TryEnterField("Value", IsSet)`(PropertyOptional.cpp:203)는 바이너리에서 bool(4 byte)을 늘 쓰고 set이면 값을 잇는다. 태그는 기준과 다를 때만 쓴다 | present u8 + 값, 필드 누락 = 기준 | 같은 배치 |
| 위치 | TArray 원소·TMap 값 허용, 컨테이너 키 불가(UhtOptionalProperty.cs:57-58), 정적 배열 불가(UhtProperty.cs:2390-2393) | 필드·멤버만 | SL `?`가 비원소·비중첩(SL 482-483행) |
| 내부 | 거의 전부(객체·컨테이너 포함), struct 재귀 금지(UhtOptionalProperty.cs:204-209) | 단일 값·String·Name·값 struct | 참조는 이미 null이 있다 |
| 비교 | set 여부 뒤 내부 비교(PropertyOptional.cpp:172-193) | wire byte | RCM 63행 |
| 텍스트 | unset `()`, set `(Value)`(PropertyOptional.cpp:344-358) | 대입 종류 Unset | AS 13절 문법, `none` 충돌 회피 |
| JSON | export: unset이면 필드 생략·하위 위치는 null(JsonObjectConverter.cpp:354-366·440-447). import: null은 그 필드를 건너뛴다(1357행) — unset으로 되돌릴 수 없다 | null = unset | 다른 결정(JSON으로 unset을 표현한다) |

### 18.6 Map·Set

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 저장소·순서 | TSet 기반(Map.h:400), 기본 sparse set(Set.h:12-42, ContainerAllocationPolicies.h:1911-1913). 제거한 칸을 free list로 재사용해 순회가 삽입 순서가 아니다(SparseArray.h:117-141·191-209) | 삽입 순서 보존 | 결정성과 Sparks 순회 규칙 |
| 패키지 저장 | `DoIntraPropertyDelta`(기본 켜짐 — Archive.h:459-462·909)면 Map은 지울 키 + 다른 항목(PropertyMap.cpp:276-289·858-915), Set은 지울 원소 + 추가 원소(PropertySet.cpp:236-384). 복제는 끔(DuplicateDataWriter.cpp:43) | 통째(B9) | RCM 64행 배열 규칙과 같게(사용자 결정 U7). key·원소 차이는 후속 후보 |
| 비교 | 개수 + permutation(PropertyMap.cpp:233-254, PropertySet.cpp:190-210) | 순서 포함 byte | 순서가 상태다 |
| 키 제한 | `CanBeContainerKey`가 없는 Array·Map·Set·Optional·Text·Interface만 거부(UhtMapProperty.cs:316-319, UhtSetProperty.cs:243-246) | 실수·참조·struct 키도 거부 | NaN·-0, 객체 정체성 변동, struct 해시는 후속 |
| 중첩 | Map 값·Array 원소로 컨테이너 금지(UhtMapProperty.cs:335-338, UhtArrayProperty.cs:257-260) | 같다 | — |
| 텍스트 | Map `((k, v),(k, v))`, Set `(a,b)`(PropertyMap.cpp:1117-1154, PropertySet.cpp:593-621), import 중복 키는 첫 값 유지(PropertyMap.cpp:1262-1270), 실패하면 맵을 비운다(PropertyMap.cpp:1208-1213) | `k:v` 목록, 중복은 오류, 실패 시 불변 | AS 13절 쉼표 목록, 엄격·원자적 입력 |
| JSON | Map → 객체(키 문자열화, 첫 글자 소문자 표준화 — JsonObjectConverter.cpp:190-226·449-453), Set → 배열(175-189) | 객체(키 그대로)·배열 | 키 표기를 바꾸지 않는다(도구 이름·Name 그대로) |
| 경로 | key 값 기반 표기 없음(PropertyPathName.h:22-30·51-55, PropertyPathHelpers.cpp:111-118), key 식별은 override 노드만(OverriddenPropertySet.h:345) | `m[k]` | CU4 안정 주소 |
| 복제(네트워크) | Map·Set 복제 불가(UhtMapProperty.cs:321-324) | 해당 없음 | 네트워크 단계에서 다시 대조 |

### 18.7 정적 배열

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 표현 | `FProperty::ArrayDim`(UT:178), 생성 코드 uint16(UObjectGlobals.h:3753) | 별도 종류 StaticArray, N ≤ 65,535 | 경로·codec 분기를 종류로 가른다 |
| 저장 | index마다 태그, index 0이 아니면 기록(CC:2002-2018, PropertyTag.cpp:464-466), 범위 밖은 경고(CC:1798-1801) | ValueArray 하나, 개수 불일치는 오류 | Record 크기·단순성, 조용한 잘림 없음 |
| 금지 | 컨테이너 정적 배열(UhtProperty.cs:2390-2393), bool 배열(:2395-2398) | 컨테이너 금지, bool 허용 | UE bool 금지는 bitfield bool 구조 때문. Rocks는 접근자 방식 |
| 텍스트 | struct 안 `Name[i]=v`(CC:3563-3589) | 쉼표 목록 | AS 13절 |

### 18.8 플래그

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| DuplicateTransient | 저장: PPF_Duplicate면 건너뜀(PR:1070). 텍스트: PPF_Copy면 제외(PR:1172-1176). OM:455, PM:204-207 | 복제 excludedFlags, Copy·PieCopy 제외 | 같다 |
| NonPIEDuplicateTransient | 저장: 복제이고 PIE가 아니면 건너뜀(PR:1076). 텍스트: PIE가 아니면 늘 제외(PR:1178-1182). OM:481, PM:226-229 | 에셋 복제 제외, Edit·Copy 제외 | 같다 |
| TextExportTransient | PPF_Copy일 때만 제외(PR:1172-1176), OM:480, PM:211-214 | Copy·PieCopy 제외 | 같다 |
| SaveGame | SaveGame 아카이브는 속성마다 SaveGame만(PR:1051-1055), 이진 struct도 멤버마다(UT:543), 전파 집합에 없음(OM:509-513), OM:458, PM:372-375 | 멤버마다 includeFlags, Atomic struct는 통째 | 같다(Atomic 통째는 UE native Serializer 결과와 같다) |
| 텍스트 입력 | `ShouldPort` 거짓 속성은 경고 후 건너뜀(PR:1664-1667) | ExcludedProperty 오류 | 엄격한 입력 |
| 필드 Deprecated | 텍스트 출력에서 늘 제외(PR:1161-1164) | 해당 없음(ID retire) | — |

### 18.9 클래스 플래그

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| Transient 저장 | 인스턴스에 RF_Transient 강제(CDO·archetype 제외 — UOG:3739-3743). SavePackage는 RF_Transient로 판정(SaveContext.cpp:124-135, CLASS_Transient 직접 검사는 주석 — 137-139). 저장 불가 객체는 export·import에서 빠지고(PackageHarvester.cpp:754-759) 참조는 null(LinkerSave.cpp:240-250). 디스크에서 읽으면 경고 뒤 로드(CDO·archetype 제외 — LinkerLoad.cpp:6146-6154) | 패키지 저장(Exclude)은 같은 결과(A1이 export에서 거르고 참조 null + 진단), 로드는 진단 뒤 로드 | 그래프 저장 집합은 호출자가 정한다(OF 5.1절) |
| Deprecated 저장 | 저장 불가(SaveContext.cpp:136-148 — 객체의 클래스로 판정), 그 Outer 아래도 함께(SaveContext.cpp:95-110), 참조 null, 편집기 로드 경고(LinkerLoad.cpp:6122-6131), NewObject 검사 없음, NotPlaceable도 켬(CS:152-155) | 같다(Exclude는 저장 때 뺌, 참조 null + 진단, 로드 진단) | 사용자 결정 U5 |
| 복제 | `FDuplicateDataWriter`는 객체 플래그 RF_DuplicateTransient·RF_NonPIEDuplicateTransient만 거르고 클래스 플래그는 보지 않는다(DuplicateDataWriter.cpp:69-79). Transient 클래스는 RF_Transient만 받는다(UOG:3740) | 저장 정책 Keep(스냅샷·undo·PIE 복제) | 같다 |
| Deprecated 클래스 참조 값 | 클래스 참조(UClass 객체)는 저장 불가 판정 대상이 아니라 그대로 저장한다 | ClassRef encode 허용, decode 진단, 쓰기·텍스트·편집기 후보는 거부 | 값 보존은 같다. 새로 고르는 경로만 막는다 |
| Deprecated 클래스를 쓰는 속성 | 비Deprecated 속성이면 UHT 오류(UhtProperty.cs:2612-2631, UhtObjectPropertyBase.cs:752-756) | RMC2209(필드 retire + migrate) | Rocks에 필드 Deprecated가 없다 |
| 상속 | CLASS_Inherit에 Transient·Deprecated(OM:292-293, CC:5177) | 같다 | — |
| NonTransient | 상속된 Transient를 끈다(CS:166-169) | 없음 | 20절 결정 15 |
| Within | 런타임 check(UOG:3480), 편집기는 경고·ensure 뒤 생성(UOG:3312-3323), Rename·PreSave 검사(Obj.cpp:270·1622-1631), 부모 Within 파생 규칙(UhtClass.cs:1424-1467), 자기 자신 허용(UhtClass.cs:1442), CDO·archetype·재인스턴스 예외(UOG:3314, Obj.cpp:1626) | 생성 InvalidArgument, Outer 없으면 거부 | Result 오류, Outer 불변 |

### 18.10 텍스트·JSON

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| API | `UScriptStruct::ExportText`·`ImportText`(CH:2443·2457), `ExportTextItem_Direct`·`ImportText_Direct`(UT:587·625) | ReflectionText 모듈 | 같은 자리(리플렉션 공용) |
| struct 형식 | `(Name=val,...)`(CC:3559-3623) | 경로 대입·JSON 객체 | AS 13절 명령행 문법 유지 |
| bool | `True`·`False`, import는 Yes·No·로컬라이즈 문자열도(PropertyBool.cpp:363·389-402) | `on`·`off`·`true`·`false` | AS 13절 |
| enum | 이름, ExternalEditor면 AuthoredName(EnumProperty.cpp:159-177) | 도구 이름 | RF 3.4절 |
| 객체 참조 | `None`, 경로(PropertyBaseObject.cpp:213-217·256-263) | resolver hook | A1·A2가 논리 경로를 정한다 |
| 배열 연산 | `.Add`·`.Remove`·`.RemoveIndex`·`.Empty`(PR:1672-1679) | 대입 종류 Erase·`a[i]={}`·`a=` | 경로 문법으로 같은 일을 한다 |
| 실패 | 부분 적용(CC:3465·3483-3529), 배열은 먼저 비움(PropertyArray.cpp:1172-1206), JSON은 첫 실패에서 되돌리지 않음(JsonObjectConverter.cpp:1357-1368) | Prepare 후 일괄 게시 | RF 원칙(쓰기 실패는 미게시) |
| 모르는 키 | 무시(PR:1658-1662). JSON `bStrictMode`는 누락 필드만 오류이고 모르는 키는 늘 허용(JsonObjectConverter.cpp:1341-1354·1371-1376) | UnknownProperty 오류 | 오타가 조용히 사라지지 않게 |
| JSON 필터 | SkipFlags 기본 Transient·Deprecated(JsonObjectConverter.cpp:404-410·429-436) | 용도별 제외(Report는 Transient) | 필드 Deprecated 없음 |
| InstancedStruct JSON | `_structType`(struct 경로)과 펼친 필드, 비면 `{}`(JsonObjectConverter.cpp:265-280) | `_structType`(타입 도구 이름), 비면 null(입력은 `{}`도) | 키 이름은 맞추고 값은 타입 도구 이름 |
| FText JSON | 기본 `ToString()`, 옵션이면 NSLOCTEXT(JsonObjectConverter.cpp:148-159) | 문자열·객체 | 지역화 형태를 보존한다 |

### 18.11 InstancedStruct·struct 상속

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 레이아웃 | `TObjectPtr<const UScriptStruct>` + 힙 메모리(InstancedStruct.h:270-271) | TypeLease + 블록 | 타입 수명을 lease로 지킨다 |
| 저장 | struct 참조 + SerialSize + 데이터, 같은 타입이면 그 기본값 대비(InstancedStruct.cpp:122-164·262-288) | Record(타입 ID 포함), 통째 | Record가 타입·길이를 담는다 |
| 미등록 타입 | 경고 후 건너뛰고 빔(InstancedStruct.cpp:233-249) | 기본 거부, 정책으로 비움 | RF 결정 9와 같은 꼴 |
| 객체 참조·중첩 | 허용 | 참조·InstancedStruct·Required 금지, 깊이 합 | 간선 모델과 고정 크기 frame 불변식 |
| Base 제한 | 메타데이터 BaseStruct, 로드 검사 없음(InstancedStruct.h:19-29). C++은 `std::is_base_of` + `checkf(IsChildOf)`(InstancedStruct.h:316-368), UHT가 BaseStruct 자동(UhtStructProperty.cs:616-636) | 쓰기·decode 검사, `static_assert(__is_base_of)` | 잘못된 데이터를 로드에서 드러낸다 |
| struct 상속 | `UStruct::SuperStruct`(CH:514), PropertyLink가 super 포함(CC:1036-1099, UT:7134-7144), USTRUCT base 하나(UhtScriptStruct.cs:1097-1101), 비USTRUCT base는 `#if CPP`로 숨김(:1100) | 펼침, 리플렉션 base 하나 + 필드 없는 비리플렉션 base | Clang이 모든 base를 본다 |
| TSubScriptStructOf | 있음(SubScriptStructOf.h:105) | 밖 | Sparks 데이터 테이블 단계 |

### 18.12 Text

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 저장 | Flags + HistoryType, Base 이력은 namespace·key·source(Text.cpp:950·962-988, TextHistory.cpp:905-912), 이력 종류 여럿(TextHistory.h:24-42) | 잎 셋 | 지역화 설계 범위를 최소로 |
| 식별·재발급 | namespace + key. 편집기 저장 때 패키지 namespace가 어긋나면 새 GUID 키를 발급하고(복제한 에셋의 키 충돌 방지), 키가 비어도 발급한다. 복제(PPF_Duplicate) 중에는 발급하지 않는다(TextHistory.cpp:868-893) | TextId(Guid), 재발급 규칙은 열린 문제 | Localization Catalog 설계 |
| invariant | HistoryType None + 문자열(Text.cpp:969·976-988) | 형태 1 | 같다 |
| 텍스트 | `INVTEXT`·`NSLOCTEXT`(Text.cpp:2052-2059, TextHistory.cpp:1151-1176) | 명령행 Invariant, JSON 문자열·객체 | AS 13절·JSON |
| 비교 | 문화 불변·지역화·빈 값·문자열 표 여부가 다르면 다름, 지역화는 identity(같은 포인터), 편집기(GIsEditor)·invariant면 표시 문자열 비교(TextProperty.cpp:63-121) | wire byte | RCM 63행 |
| Sparks | — | Text는 저장 불가(SL 1115행) | B9l은 C++ 에셋 데이터(UI CU8)용 |

### 18.13 bitfield

| 항목 | UE 5.8 | Rocks | 다른 이유 |
|---|---|---|---|
| 접근 | FieldSize·ByteOffset·ByteMask·FieldMask(UT:2596-2603), mask 읽기·쓰기(UT:2682-2709) | 생성 접근자 | raw offset 비공개 |
| 생성 코드 | `X_SetBit(void*)`로 비트를 세우고(UhtBoolProperty.cs:322-332, params :220-233) 0 버퍼에서 바이트를 찾음(PropertyBool.cpp:98-117) | `Get_`·`Set_` 정적 함수 | 멤버 포인터 불가 문제를 다른 방식으로 |
| 형식 | 폭 1만(UhtPropertyParser.cs:1047-1053), uint8·16·32·64는 uint8 크기 bool(UhtByteProperty.cs:252-262, UhtUInt32Property.cs:72-78), `bool : 1` 거부(UhtBoolProperty.cs:340-343) | 같다 | 사용자 결정 U5 |

## 19. A 단계와 다른 소비자

| 소비 단계 | 쓰는 것 | 하위 단계 | 근거 |
|---|---|---|---|
| A1 | 저장 불가 객체(Transient·Deprecated)의 export 제외(`IsUnsaveableObject`)·참조 null(저장 정책 Exclude), 로드 진단 | B9h2 | AS 150·154행, 9.2절 |
| (CU4·PIE·스냅샷) | 저장 불가 객체 보존(저장 정책 Keep) | B9h2 | 9.2절 |
| A1 | weak 참조 저장 정책(Admit의 Null) | B9d1 | OF 5.3절 |
| A1 | decode 정책 선택(`unknownClass`·`unknownStructType`) | B9d2·B9j | RF 결정 9와 같은 자리 |
| A1 | Tags 키 방식(플래그면 예약 비트 512) | (B9h 비트만) | AS 169행 |
| A2 | `asset-new`·`asset-set`·`asset-info` 값 표기, resolver, 대입 묶음(staged 대상 포함), 용도 | B9i3 | AS 571-574행 |
| A2 | `asset-duplicate`의 DuplicateTransient 계열 제외 | B9h1 | AS 97행 |
| A2 | Map 항목 대입·제거(`prop[k]=v`, `--erase`) | B9f3·B9i3 | 1.5·10.4절 |
| A2·A4 | 타입 도구 이름(`asset-new <Type>`, `--type`) | B9d2 | AS 571·573행 |
| A4 | TargetProfile·CookSettings JSON 로드(위치 진단, 재정의 병합) | B9i4 | AS 301-302행 |
| A5 | Atomic 차이 단위(미러 16종), Transform 멤버 단위 | B9b | RCM 62·64행 |
| A5 | `Downscale` 플랫폼별 값(`OrderedMap<Name, float>`) | B9f2·B9c2 | AS 451행 |
| A5 | `OodleTextureSdkVersion`(UE FName, 계약) | B9c2 | AS 453행 |
| A5 | SRGB 함께 저장(묶음 들여다보기·덧붙이기), override 경로 기록 | B9i3 | AS 408행, RCM 62행 |
| A5 | 숨김 열거자 → InvalidSetting | B9i3 | AS 435행 |
| A6 | 검증 보고 출력(선택 — JsonWriter 직접 출력도 된다) | B9i4 | AS 1절 A6 |
| (World) | 부착 계층 영속화 재검토(결정 21), 외부 객체 패키지의 weak import | B9d1 | RF 1023행 |
| (Sparks) | `T?`(같은 저장 단위 안 weak)·`Map`·`Set`·`Class<T>`·`@tableRow`(`OrderedMap<Name, InstancedStruct<>>`), Tag → Name, TagSet → `OrderedSet<Name>`, `Row<T>` → Name(`Tag?`·`Row<T>?`는 None Name — Optional<Name>을 쓰지 않는다, SL 1198행 SD19) | B9c·B9d·B9e·B9f·B9j | SL 311·481-484·495·1090·1115행 |
| (UI) | CU4 map key 경로(인코딩 가능한 key 단계), CU8 Text(DeepClone) | B9f3·B9l | UI 282·286행 |

AS 514행 MinMagFilter·MipFilter는 닫힌 값 집합이므로 enum(B3)으로 충분하다(Name 소비자가 아니다). B9j·B9k·B9l·B9m은 A 단계 소비자가 없지만 사용자 결정대로 A0-G 앞에서 끝낸다.

## 20. 결정 기록과 열린 문제

**사용자 결정**

| 번호 | 날짜 | 결정 |
|---|---|---|
| U1 | 2026-11-15 | 범위: 공백 분석의 B9a–B9l 전부 + bitfield bool(B9m). TLazyObjectPtr 제외. 분석 4절 항목(CU1·2·3·5·6·9, Replicated, Instanced, Config, CDO, 객체 단위 EditorOnly, Sparks 바인딩, custom Serialize)은 밖 |
| U2 | 2026-11-15 | 이름 "리플렉션 B9"(하위 단계 B9a… 순서 = 의존 순서), 위치 A0-O 직후·A0-G 앞, 기준 UE 5.8, 하위호환 없음 |
| U3 | 2026-11-15 | ObjectRefMap(B9f4)을 B9에 포함 |
| U4 | 2026-11-15 | Core Transform3f·Transform3d와 미러를 더하고 Atomic. 혼합 정밀도 `Transform`은 UE FTransform처럼 멤버 단위. 미러 17종 중 16종 Atomic |
| U5 | 2026-11-15 | UE와 같게: bitfield는 부호 없는 정수 4종(uint8·16·32·64)의 폭 1을 받고 `bool b : 1`은 거부, SaveGame은 멤버마다 표시, Deprecated 클래스 객체는 패키지 저장 때 뺀다 |
| U6 | 2026-11-15 | 컨테이너 이름 OrderedMap·OrderedSet(주석에 "삽입 순서 — 키 정렬 아님") |
| U7 | 2026-11-15 | Map·Set·정적 배열의 차이 단위는 B9에서 통째(key·원소 단위는 A5·prefab 후보), Name은 Core Name 규칙·차이 저장 같음은 표시 byte, 명령행 Text는 Invariant만 |

U5 경위(2026-11-15): 개정 지시의 "`uint8 b : 1`만"은 조정자가 선택지 설명에 잘못 쓴 문구였고, 사용자가 고른 기준은 "UE처럼"이다. UHT는 uint8·16·32·64 폭 1 bitfield를 모두 uint8 크기 bool로 받는다(UhtUInt32Property.cs:76-79 등 — 18.13절, 조정자 확인).

U5의 Deprecated 저장 범위: UE의 저장 불가 판정은 패키지 저장 규칙이고 복제는 클래스로 거르지 않으므로(18.9절), "저장 때 뺀다"는 패키지 저장(저장 정책 Exclude)에 적용하고 스냅샷·undo·PIE 복제(Keep)는 보존한다(조정자 결정 2026-11-15).

**설계 결정(교차검증 반영, 권고안으로 진행)**

1. 종류 성질 표는 MetaSchema constexpr 하나이고, MetaCompiler가 생성하는 대조 헤더를 ToolTests가 컴파일해 C#·C++ 표를 양방향으로 대조한다(1.3절).
2. Rocks `Atomic`은 통째 단위이며 UE `immutable`·native 결과를 따른다. UE `Atomic`의 깊은 전체 쓰기에 대응하는 의미는 없다(3.1절).
3. Name 복원은 1단계 수집 → 2a 일괄 intern → 2단계 소비. pool 한도 실패는 2a 실패다(4.1절).
4. Name 상한은 UTF-8 3,072 byte(UE 이름 전부 수용).
5. WeakRef는 같은 그래프 안만 보존하고, 정책별 처리는 5.1절 표다. Include는 지연 덮어쓰기로 순서와 무관하게 한다.
6. ClassRef의 영속 값은 TypeId, 검사는 쓰기·encode·decode 1단계, 미등록은 정책(키 위치는 늘 거부).
7. 타입 도구 이름 하나를 asset 명령·ClassRef·InstancedStruct·Sparks가 같이 쓴다(5.2절).
8. Optional wire는 전용 tag(present + 값), 위치는 필드·멤버만, 내부에서 참조 제외(6절).
9. Map·Set은 Core OrderedMap·OrderedSet, 키 제한(실수·참조·struct 제외), 순서 포함 byte 같음, 키 위치는 값 대체 정책 없음(7절, 1.7절).
10. 경로 key는 wire payload를 담는 `PathKey`, 문자열은 따옴표 없는 schema 해석(1.5절). 진단 경로는 삽입 순번(1.6절).
11. 정적 배열은 정확히 N, ObjectRef 원소 허용(8절).
12. 새 필드 플래그는 전파하지 않는다(UE와 같다). 포함 필터는 멤버마다이고 Atomic struct만 통째다(9.1절).
13. 저장 불가 객체(Transient·Deprecated와 부분 트리)는 저장 옵션 `unsaveable`로 다룬다. Exclude(기본, 패키지 저장)는 저장 집합에서 거부·참조 null + 진단이고 A1이 export에서 거른다. Keep(스냅샷·undo·PIE 복제)은 보존한다. 로드는 Transient·Deprecated 노드 모두 진단 뒤 로드한다(9.2절, 조정자 결정 2026-11-15 — UE 5.8 동작).
14. Deprecated 클래스를 가리키는 필드는 RMC2209(필드 retire + migrate).
15. UE `NonTransient` 대응은 두지 않는다. Rocks의 다른 상속 플래그(WorldOnly)도 끄는 지정자가 없고, Transient 클래스는 런타임 도우미 타입이라 저장 가능한 파생 수요가 아직 없다. 필요해지면 형식 변경 없이 지정자로 더할 수 있다.
16. Within은 `OuterSummary` 타입 칸과 생성 정체성 규칙의 한 단계로 판정하고, 철자는 Binds처럼 Clang으로 푼다(9.2절).
17. ReflectionText는 새 모듈, 용도별 출력·입력 제외 플래그(UE `ShouldPort` 대응, Edit 입력은 Transient만 제외), 대입 묶음은 Prepare → 들여다보기·덧붙이기 → Publish. Prepare·Append가 새 대상을 고정하고, Publish는 변경 표지 확인 → 묶음 구간에서 대상 재확인·관찰자 묶음 예약 → 무할당 게시다. 그래서 게시 단계는 실패하지 않는다(10절, 조정자 결정 2026-11-15).
18. 명령행 대입 종류 Set·Unset·Erase, `none`은 참조·ClassRef·InstancedStruct null 전용, Flags는 `+`, escape 표는 하나(10.4절).
19. JSON import는 struct 병합·그 밖 통째 교체, Map은 선택 `MergeByKey`(10.4절).
20. InstancedStruct 내용은 참조·Registry 컨테이너·InstancedStruct·Required가 없는 struct, 위치 깊이 + 내용 깊이 ≤ 16(11절).
21. struct 상속은 펼침, 리플렉션 base 하나 + 필드 없는 비리플렉션 base, struct 멤버 thunk는 소유자 명시(12절).
22. Text는 잎 셋 + 형태 byte, Registry 경계·FieldValue 반출은 DeepClone(13절).
23. bitfield는 생성 접근자, offset 없음, NATIVE bitfield 허용(14절).
24. ClassRef의 Deprecated 거부는 쓰기·텍스트·편집기 후보에만 둔다. encode는 허용하고(로드한 값을 다시 저장할 수 있다) decode는 `DeprecatedType` 진단을 낸다(5.2절, 조정자 결정 2026-11-15 — UE도 클래스 참조 값을 저장한다).
25. 대입 묶음의 준비 단위는 셋(참조 없는 값 부분 트리, 단일 참조 위치의 간선 변경, Registry 컨테이너의 새 버퍼·간선 차이)이고, 게시는 간선마다 장벽·관찰자 기록을 적용한다(10.2절).
26. 값이 같은 ReadOnly 대입은 무동작이고 override 경로에 넣지 않는다. `asset-info` → `asset-set` 왕복은 값 보존을 뜻한다(10.1·10.4절).
27. Admit에서 weak는 slot 캐시를 쓰지 않고, hook의 Import 응답은 null + 진단으로 받는다(5.1절).

**열린 문제(이 단계 밖에서 정한다)**

- weak import(로드를 강제하지 않는 외부 weak): World 외부 객체 패키지(UE OFPA 대응)를 만들 때 필수다. UE는 같은 outermost의 다른 패키지와 쿠킹에서 weak 대상을 import로 보존한다(PackageHarvester.cpp:1106-1115). 요구 주체는 World·Sparks다.
- TextId 정체성: 복제 때 유지·재발급, 누가 발급하는가(UE는 편집기 저장 때 재발급 — TextHistory.cpp:868-893). 지역화 설계(gather 파이프라인)와 함께.
- Map·Set·정적 배열의 key·원소 단위 차이 저장(A5·prefab).
- 패키지 name 표(Name 문자열 중복 제거): A1이 크기를 재고 정한다.
- struct 키(생성 해시·같음)·실수 키: 소비자가 생길 때.
- InstancedStruct가 참조 struct를 담는 Registry 관리 변형, `TSubScriptStructOf` 대응(Sparks 데이터 테이블 단계), `SoftClassRef`(Sparks 단계).
- Text의 문자열 표·Format 이력 영속.
- A5 클래스 기본값·prefab의 Transient·Within 템플릿 예외(UE CDO·archetype 예외).
- CU4의 소유형 경로(`OwnedPropertyPath`)와 Record 형식.
- "Sparks 저장 가능 대응 ⊆ 성질 표" 대조 시험(Sparks 단계).
- 사용 중인 Registry 사이 값 복사 일반 API(Text DeepClone 외 Name 토큰 pool 일치 등)는 PIE 복제 설계와 함께.
- `asset-info` 출력 재입력의 override 기록: 값이 같아도 override 경로로 기록되어 A5 뒤 기본값 전파가 끊긴다. A5가 기준 차이 출력과 함께 정한다(재입력 시 기준과 같은 값을 기록하지 않는 옵션 등).
- Sparks 필드·struct 멤버의 SaveGame 표시 주체: SL `@save`(SL 224행)는 영속 표시다. 결정 U5(멤버마다 표시)에서 Sparks struct 멤버를 누가 SaveGame으로 표시하는지는 Sparks 단계가 정한다.
- PIE 복제에서 Transient 클래스 객체의 처리: B9는 Keep(보존)으로 정했다. PIE가 그 객체를 다시 만드는 쪽을 원하면 PIE 복제 설계에서 정한다.

## 21. 문서 반영 목록

**이 문서를 넣을 때 함께 고친 곳**(아직 구현하지 않은 내용은 "리플렉션 B9가 더한다·바꾼다"는 연결로 적었다)

| 문서 | 위치 | 고칠 내용 |
|---|---|---|
| RF | 3행 머리 | 리플렉션 B9 문서 연결 |
| RF | 2.1절(109행) | 타입 지정자 `Name="…"`·`Within="…"` 값 규칙(Clang 해석) |
| RF | 2.2절 표(113-120행) | Atomic·Transient·Deprecated·Within·타입 `Name` 행 또는 이 문서 연결 |
| RF | 2.3절 표(128-134행) | 필드 플래그 4종, Required 허용 범위(ClassRef 등)와 DuplicateTransient 조합 거부 |
| RF | 3.4절 185행 | enum underlying 8종(RMC2401 뜻 변경) |
| RF | 3.5절 194-217행 | ValueKind 목록·`FieldSchema.key`·등록 규칙·키 대상 |
| RF | 4.1절 242행, 4.6절 | 상속 금지 → 12절, 깊이 규칙에 InstancedStruct 합, struct 등록의 base·Atomic mask |
| RF | 5.1절 382행 | 배열 원소 종류에 Int8·Int16·Name·WeakRef·ClassRef·InstancedStruct·Text |
| RF | 5.4절(397-403행) | wire 14–18, 원소 tag 허용 목록 |
| RF | 6.1·6.2·6.4절 | 참조 문맥의 weak 쓰기·읽기, 2a Name intern, 관찰 struct의 Name 제한, 진단 경로의 삽입 순번 |
| RF | 7절 647·650행, 7.1절 | `PathStep` 선택자·key 단계, "도구 계층" → ReflectionText, 대입 묶음 게시(`BeginBatchPublish` — 대상 고정·재확인·관찰자 예약·준비 단위), 새 종류 경로 연산 |
| RF | 10절 표(956-974행) | 15절 진단 |
| RF | 11절 | 리플렉션 B9 단계 연결 |
| RF | 12절 결정 4(1006행)·21(1023행)·22(1024행) | B9k로 열림, WeakRef PROPERTY 가능(계층 결정은 World), struct 차이 단위는 Atomic, 세이브게임 "전부" = 기준 차이 없음·필드 범위는 SaveGame |
| RF | 12절 열린 문제 1032·1033·1036행 | TMap·TSet, FText·클래스 참조, 배열 기능(정적 배열·원소)을 지우고 이 문서로 연결. `ScriptName`·`SoftClassRef`·`EditFixedSize`·Instanced·overridable 배열은 남김 |
| RCM | 18행·81행(2단계) | `ClassRef<Base>` 영속 값 타입은 B9d, 등록 핸들 캐시는 2단계 |
| RCM | 62·64·70행 | 3.3절 정정 문구 |
| RCM | 65행 | 복제는 DuplicateTransient 계열을 뺀다(override 집합도), 세이브게임 필드 범위는 SaveGame |
| OF | 1.2·1.3절 | `RegistryOptions::names`(NamePool 빌림·수명), `ObjectAllocator()` |
| OF | 1.5절 정체성 규칙 6–9 | Within 단계, `OuterSummary` 타입 칸 |
| OF | 4절 | includeFlags(멤버마다)와 2c 제외 |
| OF | 5.2·5.3절 | 저장 옵션 `unsaveable`(Exclude·Keep), 정책별 weak 처리 표(Admit의 weak는 slot 캐시 없음·Import는 null), Include 지연 덮어쓰기(지연 항목·메모리 청구), `ExternalReferenceInfo::weak`와 예시 hook의 weak 분기, 저장 불가 객체 참조 null(진단 값 3·4) |
| OF | 5.4절 | Transient·Deprecated 노드 로드 진단·그래프 안 Within·Name 수집과 2a 몫 청구, 2a의 Name intern·기준 Outer Within |
| OF | 5.5절 | weak 간선은 도달 검사에 넣지 않음 |
| OF | 5.6절 | Set·Map·StaticArray 원소, Map 값·Optional·InstancedStruct Record의 누계 |
| OF | 5.7절 | 2a Name pool 한도, Transient·Deprecated·Within 오류 위치 |
| AS | 1절 단계 표 | A0-O와 A0-G 사이에 "리플렉션 B9" 행 |
| AS | 5.1절, 150·154행 | export 집합 구성이 `CollectInners` 결과를 `IsUnsaveableObject`로 거름(저장 정책 Exclude — `CollectInners`는 그대로), 패키지 안 weak 저장 규칙 |
| AS | 6.1절 233행 | 관찰 대상에 ObjectRefMap 값 |
| AS | 301-302행 | TargetProfile·CookSettings JSON은 ReflectionText(B9i), 재정의 병합 |
| AS | 451·453행 | Downscale은 Map + Name, OodleTextureSdkVersion은 Name 필드 |
| AS | 13절(571-600행) | 타입 도구 이름, `--unset`·`--erase` 목록 옵션, Map `k:v`(첫 `:`), Flags `+`, key 경로 문법, escape 표, asset-info 출력(`<경로>=<값>`·`--json`·`--all`) |
| SL | 20·482-484행 | 컨테이너 = OrderedMap·OrderedSet, `Class<T>` = ClassRef, 타입 도구 이름, 키 제한(실수 키 불가) |
| SL | 24·213·495행 | 객체 `T?` 필드 = 같은 저장 단위 안 WeakRef, 셀 사이는 World 영속 참조 |
| SL | 1115·1198행 | 저장 가능 타입의 C++ 대응(Tag → Name, TagSet → OrderedSet<Name>, Row<T> → Name, `?`는 None) |
| UI | 275·286행(CU8) | 낡은 "bool·정수·실수·Guid·String·ObjectRef만" 서술 정정(enum·struct·배열·Soft는 B3–B5, Name·Text·Map 등은 리플렉션 B9) |
| UI | 282행(CU4) | map key 경로는 B9f3(인코딩 가능한 key 단계) |
| EIP | 2.3절(133-139행) | authoring LocalTransform은 `Transform`(혼합), Transform3f·Transform3d는 같은 실패 규약의 단일 정밀도 값 타입 |
| EngineRuntime | 130·271행 | 부착 계층 "WeakObjectRef 뒤 저장 안 됨" → WeakRef PROPERTY 가능, 결정은 World 단계 |
| CoreDataFoundations | Json 절 | `JsonWriter::Float`, `JsonValue::Position` |
| Implementation/ReflectionMetadata.ko.md(RM) | 지정자 표(85-100행 부근)·struct 절(141-145행)·descriptor 절(198-202행)·65·67행 | 새 지정자·descriptor 키·PROPERTY 거부 목록·상속 규칙(단계별 구현 때) |
| Implementation/Serialization.ko.md | 55행 부근 | 새 wire·ValueArray 원소 확장 |
| Implementation/HashCollections.ko.md | — | OrderedMap·OrderedSet |
| Implementation/Name.ko.md·NameTextImplementation.ko.md | Name.ko.md 35행 | "직렬화 연결은 별도 정의" → Name 필드 영속 규칙(4절), `TryFind`·`NameTextEquals` |
| Implementation/ObjectLifecycle.ko.md | 생성 규칙 | Within·저장 불가 클래스 |
| Implementation/FoundationStatus.ko.md | — | 리플렉션 B9 상태 |
| 코드 주석 | `CORE/Math/Transform.h` 머리, `TypeRegistry.h` struct 플래그 주석 | B9b1·B9b2 커밋에서 |

**다른 세션에 넘김(이 세션은 고치지 않는다)**

- Docs/Design/ShaderSystem.ko.md 98·101·268·653행: 고정 순서 "A0-B → A0-O → A0-G → A1–A6 → 화면 마일스톤"과 상태 서술에 리플렉션 B9(A0-O 다음, A0-G 앞)를 넣는다. 셰이더 세션 소유 문서다.

**조사 기록 정정**

- 이 단계 착수 전 공백 분석(저장소 밖 조사 문서)의 Atomic 서술 "CC:1984-1988 — 하나라도 다르면 멤버 전부"는 틀렸다(그 문서도 고쳤다). UE Atomic 태그 저장은 같은 멤버를 건너뛴다(CC:2016-2029). 수학 타입의 통째 저장은 native Serializer·Immutable 이진 경로가 만든다(18.2절).

## 22. 리뷰 반영 기록

교차검증 A(UE 대조)·B(Rocks 정합)·C(소비자)의 지적과 처리다. "반영"은 해당 절에 설계로 넣었다는 뜻이다.

| 지적 | 요지 | 처리 |
|---|---|---|
| A H1 | 텍스트 복사 출력의 DuplicateTransient·NonPIEDuplicateTransient 제외 누락 | 반영: 용도별 제외(10.1절 표), 18.8절 |
| A M1 | Rocks Atomic ≠ UE Atomic | 반영: 3.1절 "UE Atomic과의 관계", 공백 분석 정정 기록(21절) |
| A M2 | Optional UE 저장 서술 오류, JSON null 차이 | 반영: 6절·18.5절 |
| A M3 | weak 외부 import의 실제 소비자(외부 객체 패키지·쿠킹) | 반영: 18.4절, 20절 열린 문제 |
| A M4 | TextId 재발급 규칙 누락 | 반영: 13절·18.12절, 20절 열린 문제 |
| A M5 | UE NonTransient | 반영: 대응 없음으로 결정(20절 결정 15), 18.9절 |
| A M6 | Deprecated 클래스를 쓰는 속성의 UHT 오류 | 반영: RMC2209(9.2절) |
| A M7 | Transient의 CDO·archetype 예외 | 반영: 9.2절(A5·prefab에서 다시 정함), 20절 |
| A L1 | FName은 정확히 ASCII만 | 반영: 4절·18.3절 |
| A L2 | NAME_SIZE는 UTF-16 단위 | 반영: 상한 3,072 byte(4절, 결정 4) |
| A L3 | Name None 출력 | 반영: `None` 출력(4절) |
| A L4 | weak 로드 불일치 정책 | 반영: InvalidArgument로 명시하고 차이 기록(5.1절·18.4절) |
| A L5 | 인용 정밀도 | 반영: 18절 인용 행 정정(CheckValidObject 913행, CC:3548-3553 등) |
| A L6 | Optional 위치 차이 | 반영: 18.5절 |
| A L7 | TSet도 원소 단위 차이 | 반영: 7.2절·18.6절 |
| A L8 | JSON 키 표준화·strict 모드 | 반영: 18.6·18.10절 |
| A L9 | Within 세부(직접 Outer, 자기 자신, 예외) | 반영: 9.2절·18.9절 |
| A L10 | 텍스트 배열 연산·Map import 실패 | 반영: 10.4절 연산 대응, 18.6·18.10절 |
| A L11 | InstancedStruct JSON `_structType` | 반영: 키 이름을 UE와 맞춤(10.4절) |
| A L12 | 비USTRUCT base `#if CPP` | 반영: 필드 없는 비리플렉션 base 허용(12절) |
| A L13 | FText 비교의 GIsEditor | 반영: 18.12절 |
| B H1 | InstancedStruct 동적 깊이 | 반영: 담긴 struct의 InstancedStruct 금지 + 깊이 합 검사, encode는 InvalidArgument(11절) |
| B H2 | 대입 묶음 게시 실패 | 반영: 변경 표지 → 관찰자 묶음 예약 → 무할당 게시, B9i2 선행 커밋(10.2절·16절). 대상 생존 재확인은 반영 확인 리뷰 H1에서 더함 |
| B H3 | Include에서 weak 결과가 순서에 좌우 | 반영: 지연 덮어쓰기와 정책별 표(5.1절) |
| B M1 | Name 2a 작업량·관찰 struct | 반영: 4.1절 |
| B M2 | Map 키 대상 칸 | 반영: 1.1절 키 대상, B9f2 |
| B M3 | 값 대체 정책으로 키 중복 | 반영: 키 위치는 늘 거부(1.7절) |
| B M4 | 원소·Map 값 자리 InstancedStruct wire | 반영: u64 길이 + 내용(1.2절) |
| B M5 | Within 판정 자리 | 반영: `OuterSummary` 타입·정체성 규칙(9.2절) |
| B M6 | 최단 실수 표기는 이미 Core에 있음 | 반영: B9i1 = `JsonWriter::Float`, 명령행은 StringBuilder `"{}"`(10.1절) |
| B M7 | InstancedStruct 안 Required | 반영: 담기 조건 `ContainsRequired` false(11절) |
| B M8 | descriptor 형식 변경 | 반영: 1.8절 |
| B M9 | 진단 경로의 Map 항목 | 반영: 삽입 순번(1.6절), frame 일반화(B9f2·B9g) |
| B M10 | struct 멤버 thunk 캐스트 | 반영: 소유자 명시 인자(12절) |
| B M11 | allocator 조회 API | 반영: `ObjectAllocator()`(1.7절, B9e) |
| B M12 | Optional FieldValue·aggregate | 반영: kind = 내부 종류 + present, 성질 표 "통째 FieldValue" 열(1.1·1.3절) |
| B L1 | `__is_base_of` 서술 오류 | 반영: `static_assert(__is_base_of)`(12절) |
| B L2 | UE weak 쿠킹 import | 반영: 18.4절 |
| B L3 | pool 없는 Name 비교 | 반영: `NameTextEquals`·분해 공개, `TryFind` 반환 규칙(4절) |
| B L4 | 세이브게임 로드의 2c | 반영: includeFlags 밖 필드 제외(9.1절) |
| B L5 | "세이브게임 전부 저장" 문구 충돌 | 반영: 9.1절, 21절(RF 결정 22·RCM 65행) |
| B L6 | RegisterStruct flags mask | 반영: 1.1·3.1절 |
| B L7 | ClassRef Deprecated 거부의 단계 | 반영: B9h2에 둠(5.2절·16절) |
| B L8 | 파생 조회는 `IsA`가 있음 | 반영: `IsA` 사용, 새 조회는 struct용 `IsStructA`만(5.2·12절) |
| B L9 | 정적 배열 NATIVE 형질 | 반영: 8절 |
| B L10 | Within 철자 해석 | 반영: Binds와 같은 Clang 해석(9.2절) |
| B L11 | ExternalReference 사유 칸 | 반영: 진단 값 0–4 정의(15절) |
| B L12 | Map `:`·escape | 반영: 첫 escape되지 않은 `:`, escape 표(10.4절), AS 13절 개정(21절) |
| B L13 | OrderedMap API 이름 | 반영: HashMap 규약(7.1절) |
| B L14 | ObjectRefMap 표 일관성 | 반영: U3으로 포함, 표가 전제와 맞음 |
| B 5절 | 대조 static_assert의 한계 | 반영: 대조 헤더 + ToolTests 양방향(1.3절), 대조 시험 행은 단계마다(8절) |
| B 7절 | Serialization 몫·대입 묶음 게시 분리·규모·전체 체인 | 반영: B9a3·B9i2 분리, B9c2·B9d1·B9m 규모 상향, B9a1 직후 전체 체인(16절) |
| B 8절 | 문서 목록 보완, ShaderSystem 소유 | 반영: 21절(다른 세션에 넘김 분리) |
| B 9절 | 범용 기능 중복 | 반영: StringSplit escape는 Core, NFC는 Core Unicode, `Hash<Name>`(10.1·7.1절) |
| B 참고 | Name 생성자 기본값 | 반영: 4절 |
| C H1 | 대입 API 두 단계 | 반영: 10.2절 |
| C H2 | ObjectRefMap 범위 | 사용자 결정 U3으로 닫힘(7.4절) |
| C H3 | Transform Atomic 근거 | 사용자 결정 U4로 대체(3.2절) |
| C M1 | JSON 재정의 병합 의미 | 반영: import 의미 표·`MergeByKey`(10.4절) |
| C M2 | JSON 의미 오류 위치 | 반영: `JsonValue::Position`, 진단 레코드, 오류 모두 모으기(10.1·10.2절) |
| C M3 | 명령행 `none` 겹침 | 반영: Unset 대입 종류·`--unset`, `none`은 null 전용(10.4절) |
| C M4 | ClassRef 텍스트 이름 | 반영: 타입 도구 이름, TypeId 32·36자 입력(5.2절) |
| C M5 | 경로 key 문법과 셸 | 반영: 따옴표 없는 key, escape 표 하나(1.5·10.4절) |
| C M6 | Name 소비자(OodleTextureSdkVersion), MinMagFilter는 enum | 반영: 19절, AS 514행 변경 삭제 |
| C M7 | 숨김 열거자 | 반영: `HiddenEnumerator`(10.1절) |
| C M8 | resolver 경계 | 반영: 10.3절 |
| C M9 | asset-info 출력·왕복 | 반영: 10.4절 |
| C M10 | PathKey 표현·CU4 | 반영: wire payload `PathKey`(1.5절) |
| C M11 | InstancedStruct 실행 중 깊이 | 반영: B H1과 함께(11절) |
| C M12 | Text allocator 수명 | 반영: `DeepClone`(13절) |
| C L1 | `@tableRow` 소비자는 B9j | 반영: 11절·19절, TSubScriptStructOf 제외 사유 정정(0절) |
| C L2 | SL Tag·TagSet·Row 대응 | 반영: 19절, 21절 SL 행 |
| C L3 | weak "같은 저장 단위" | 반영: 5.1절, 요구 주체 World·Sparks(20절) |
| C L4 | struct 멤버 SaveGame 거부 | 반영하지 않음: 사용자 결정 U5로 멤버마다 표시가 규칙이 되어 멤버 SaveGame이 정상 표기다 |
| C L5 | Required + DuplicateTransient | 반영: RMC2202(9.1절) |
| C L6 | Flags `|`와 셸 | 반영: 명령행 `+`, JSON 이름 배열(10.4절) |
| C L7 | Atomic 되돌리기 구현 시점 | 반영: RCM 70행 후속, B9b는 단위만(3.1절) |
| C L8 | TextExportTransient 입력 | 반영: Edit 입력은 받음, Copy만 거부(10.1절) |
| C L9 | struct 값 Map의 새 항목 | 반영: `m[k]={}`·`InsertDefault`(7.3·10.4절) |
| C L10 | 복제 override 집합 | 반영: 9.1절 |
| C L11 | A6는 선택 소비자 | 반영: 19절 |
| C L12 | 쿠킹 접기 수단 | 반영: `SubstituteStruct`, Downscale은 빌드 입력(7.2절) |
| C L13 | Sparks 컴파일러의 성질 표 사용 | 반영: 1.3절, 대조 시험은 Sparks 단계(20절) |
| 사용자 결정 U5(bitfield) | `uint8 b : 1`만 | 조정자 확인(2026-11-15): "uint8만"은 선택지 설명의 잘못된 문구였고 기준은 "UE처럼"이다. 부호 없는 정수 4종으로 확정(20절 U5 경위) |

**반영 확인 리뷰(1회)** — 차단 0·높음 1·중간 7·낮음 14와 조정자 결정(2026-11-15)의 처리다.

| 지적 | 요지 | 처리 |
|---|---|---|
| H1 | Prepare와 Publish 사이 대상 사망으로 게시 중 부분 적용 | 반영: Prepare·Append가 새 Hard·Weak 대상을 RootRef로 고정, Publish 2단계(묶음 구간 안)에서 모든 대상의 생존·기대 타입·Registry를 다시 확인한 뒤에만 게시. 그래서 3단계는 실패하지 않는다(10.2절). 시험 추가(17절 B9i) |
| M1 | 준비 사본의 참조 처리(장벽·관찰자·주소 검사 우회) | 반영: 준비 단위 셋(값 부분 트리·단일 참조 위치 간선 변경·Registry 컨테이너 새 버퍼와 간선 차이), 게시 때 간선마다 장벽·관찰자 기록, 키 예약은 간선 목록에서 셈(10.2절) |
| M2 | Include weak 지연 진단의 경로 보존 | 반영: 지연 항목에 objectIndex·경로 사본(≤ 17단계, 삽입 순번), 메모리를 그래프 바이트·작업량에 청구, 진단은 encode 뒤 기록 순서로(5.1절) |
| M3 | ClassRef encode의 Deprecated 거부로 재저장 불가 | 반영(조정자 결정): encode는 허용, decode에서 `DeprecatedType` 진단, 쓰기·텍스트·편집기 후보만 거부(5.2절, 18.9절) |
| M4 | 저장 불가 규칙을 모든 그래프 저장에 강제 | 반영(조정자 결정): 저장 옵션 `unsaveable` = Exclude(기본, 패키지 — UE SavePackage)·Keep(스냅샷·undo·PIE 복제 — UE 복제는 클래스로 거르지 않음, DuplicateDataWriter.cpp:69-79)(9.2절) |
| M5 | asset-info → asset-set 왕복과 ReadOnly·override 기록 | 반영: 값이 같은 ReadOnly 대입은 무동작·override 경로 제외, 왕복은 "값 보존"으로 좁힘, 재입력의 override 기록은 A5 열린 문제(10.1·10.4·20절) |
| M6 | bitfield 결정 문구와 다름 | 조정자 결정으로 원고대로 확정(부호 없는 정수 4종, `bool b : 1` 거부). U5 경위를 20절에 기록 |
| M7 | Transient 노드 로드 거부는 복구 경로 없음 | 반영(조정자 결정): 로드하고 `TransientType` 진단, Exclude 재저장에서 빠짐(9.2절, 18.9절 — LinkerLoad.cpp:6146-6154) |
| L1 | Edit 입력의 NonPIEDuplicateTransient 거부 | 반영: 출력·입력 제외를 나눔, Edit 입력은 Transient만 제외(10.1절) |
| L2 | 2c 제외 문구 "최상위" | 반영: "최상위 필드·멤버"(9.1절) |
| L3 | 진단 코드 겹침 | 반영: 컨테이너 쪽 위치 규칙 우선, 사례별 코드 하나(15절) |
| L4 | Admit의 weak Import·slot 캐시 | 반영: slot 캐시 없이 출현마다 질의, Import는 null + 진단, hook 계약에 weak 분기(5.1절) |
| L5 | resolver 호출 시점과 Append 진단 | 반영: Prepare·Append, Append 진단은 Prepare의 TextDiagnostics로(10.2·10.3절) |
| L6 | TextCodecOptions 내용·입력 수명 | 반영: 구조체 정의(use·resolver·names·mapMerge·maxDiagnostics), Prepare가 문자열 복사(10.2절) |
| L7 | Map 중복 규칙·판정 주체 | 반영: reader는 형식만, 중복은 codec 1단계가 Set·Map 모두(1.2절) |
| L8 | key 안 `=` | 반영: `=`를 따옴표 조건에, 대입은 `[...]`·따옴표 밖 첫 `=`에서 분해(1.5·10.4절) |
| L9 | `Mat4f` 이름 | 반영: `Mat4`(Matrix4.h:316-317)(3.2절) |
| L10 | B9j 시험 문구 | 반영: Emplace 거부와 깊은 위치 깊이 합 위반으로 고침(17절) |
| L11 | B9f 시험의 InstancedStruct·Text 값, 8절 문구 | 반영: 시험을 B9j·B9l로 옮기고 8절을 "Array·StaticArray 원소와 Map 값까지"로(8·17절) |
| L12 | B9f2·B9h2 선행에 B9d1 | 반영(16절) |
| L13 | RMC2207과 NATIVE | 반영: PROPERTY·Binds 미러에만, NATIVE는 정수·bool 모두 허용(14·15절) |
| L14 | 낡은 용어·위치·행 | 반영: PathWriteBatch → 대입 묶음 게시, FieldCodecHooks.h, LinkerLoad 6122-6131·6146-6154, `CollectInners`는 그대로이고 A1이 `IsUnsaveableObject`로 거름(1.6·4.1·9.2·18.9·21절) |
| 참고 1 | MutationStamp와 C++ 직접 쓰기 | 반영: 계약 한 줄(10.2절) |
| 참고 2 | Sparks SaveGame 표시 주체 | 반영: 20절 열린 문제 |
