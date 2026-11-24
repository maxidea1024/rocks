#pragma once

#include <Base/Guid.h>
#include <Core/Assert.h>
#include <Core/Containers/Span.h>

namespace rocks {

using TypeId = Guid;
using ContextId = Guid;

// flags는 저장 형식과 별개인 메타데이터 정책이다. 알 수 없는 비트는 등록 단계에서 거부한다.
//
// TypeFlags(클래스는 Abstract·WorldOnly, struct는 Atomic만 — 대상이 아닌 비트는 등록에서
// 거부된다):
//   Abstract  — 클래스. 등록할 수 있으나 생성할 수 없다(construct가 null).
//   WorldOnly — 클래스. World 쪽에서만 쓰는 타입이다. 상속된다(base가 WorldOnly면 파생도 이
//               플래그가 있어야 등록된다). 에셋 객체 Registry의 생성 거부는
//               RegistryPolicy::rejectWorldOnly(Object/ObjectRegistry.h)다.
//   Atomic    — struct(ROCKS_STRUCT(Atomic)). 값이 통째 단위다: 차이 저장은 멤버 하나라도
//               기준과 다르면 모든 멤버를 쓰고, override 집합·되돌리기의 단위가 그 struct 필드
//               경로다(Docs/Design/ReflectionExtensions.ko.md 3.1절 — 차이 쓰기·override·
//               되돌리기 구현은 A5, 되돌리기는 RuntimeClassMetadata 'struct 통째 되돌리기' 후속과
//               함께). 로드·경로 접근·저장 형식은 바뀌지 않는다. Atomic struct 안의 비Atomic
//               멤버도 통째로 쓰이고, 비Atomic struct 안의 Atomic 멤버는 그 멤버 경로가 단위다.
//               Core 값 타입 미러는 혼합 정밀도 Transform을 뺀 16종이 Atomic이다. 값 16은 클래스
//               플래그 예약 비트(4·8)를 비켜 둔다(ReflectionExtensions.ko.md 1.4절).
//   Transient·Deprecated — 클래스(ROCKS_CLASS(Transient)·ROCKS_CLASS(Deprecated) — 리플렉션 B9h2,
//               Docs/Design/ReflectionExtensions.ko.md 9.2절). 상속된다(파생도 같은 비트를 가져야
//               한다 — 등록 InvalidArgument). 이 클래스 객체는 저장 불가 객체다: 그래프 저장 정책
//               UnsaveablePolicy::Exclude(패키지 저장 — 기본)가 그 객체를 저장 집합에서 거부하고
//               그것을 가리키는 참조를 null + 진단으로 쓴다. 로드는 되고
//               진단(TransientType·DeprecatedType)을 낸다. Deprecated 클래스는 ClassRef 쓰기가
//               거부한다(이미 저장된 값은 진단과 함께 읽고 그대로 저장한다).
//   EditorOnly — 클래스(ROCKS_CLASS(EditorOnly) — Docs/Design/AssetSystem 10.2절 A4e2a, UE
//               UObject::IsEditorOnly() 재정의 대응). 쿠킹 데이터에 없는 클래스다. 상속된다(파생도
//               같은 비트를 가져야 한다 — 등록 InvalidArgument). 대표 타입이 EditorOnly인 에셋은
//               쿠킹 대상이 아니고 쿠킹 패키지 쓰기는 이 클래스 객체를 저장 불가 객체처럼 뺀다.
//               값 32는 Atomic(16) 다음 비트다.
enum class TypeFlags : uint32 {
    None = 0,
    Abstract = 1,
    WorldOnly = 2,
    Transient = 4,
    Deprecated = 8,
    Atomic = 16,
    EditorOnly = 32
};

// 저장 불가 클래스 비트(Transient·Deprecated — 리플렉션 B9h2)다.
inline constexpr uint32 UnsaveableTypeFlags =
    static_cast<uint32>(TypeFlags::Transient) | static_cast<uint32>(TypeFlags::Deprecated);

// 상속되는 클래스 비트다(base에 있으면 파생에도 있어야 한다 — 등록 InvalidArgument).
inline constexpr uint32 InheritedTypeFlags = static_cast<uint32>(TypeFlags::WorldOnly) |
                                             UnsaveableTypeFlags |
                                             static_cast<uint32>(TypeFlags::EditorOnly);

// FieldFlags:
//   Transient  — GC 추적은 하되 저장 그래프에서 뺀다.
//   ReadOnly   — 일반 write를 거부하고 생성·복원의 staged write만 허용한다.
//   EditorOnly — 항상 컴파일되고 플래그만 단다. 쿠킹 데이터에서 빼는 것은 graph v2(A0-O)다.
//   Required   — typed 참조·소프트 참조 필드와 그 원소 배열(ObjectRefArray,
//                Array<SoftObjectRef>)에만 쓴다. 배열이면 원소마다 non-null이라는
//                뜻이다(비어 있지 않아야 한다는 뜻은 없다). null 거부는 패키지
//                저장·쿠킹(A1)이고 로드는 진단만 한다.
// 필드·struct 멤버 플래그다. 16 이상은 리플렉션 B9h1의 필드 플래그다(Docs/Design/
// ReflectionExtensions.ko.md 9.1절): DuplicateTransient는 복제(에셋·Tryout) 저장과 복사 텍스트에서,
// NonTryoutDuplicateTransient는 Tryout이 아닌 복제와 텍스트에서, TextExportTransient는 복사
// 텍스트에서 뺀다(복제·텍스트 호출자가 excludedFlags로 고른다). SaveGame은 세이브게임 저장의 포함
// 필터 (includeFlags — 필드·멤버마다 자기 플래그로 판정)다. Required와 DuplicateTransient·
// NonTryoutDuplicateTransient는 함께 쓸 수 없다(복제본이 늘 null — 등록 InvalidArgument,
// MetaCompiler RMC2202).
enum class FieldFlags : uint32 {
    None = 0,
    Transient = 1,
    ReadOnly = 2,
    EditorOnly = 4,
    Required = 8,
    DuplicateTransient = 16,
    NonTryoutDuplicateTransient = 32,
    SaveGame = 64,
    TextExportTransient = 128
};

constexpr bool HasFlag(uint32 flags, TypeFlags flag) noexcept {
    return (flags & static_cast<uint32>(flag)) != 0;
}

constexpr bool HasFlag(uint32 flags, FieldFlags flag) noexcept {
    return (flags & static_cast<uint32>(flag)) != 0;
}

// 바깥 필드(struct 필드·배열 필드)의 플래그 중 안쪽으로 전파되는
// 것(EditorOnly·Transient·ReadOnly)과 안쪽 필드 자기 플래그를 합친 유효
// 플래그다(Docs/Design/ReflectionFoundations.ko.md 6.4절). 최상위 필드의 유효 플래그는 자기
// 플래그다. 직렬화(제외·Required)와 경로 쓰기(ReadOnly)가 같은 규칙을 쓴다.
constexpr uint32 PropagateFieldFlags(uint32 parent, uint32 own) noexcept {
    constexpr uint32 inherited = static_cast<uint32>(FieldFlags::EditorOnly) |
                                 static_cast<uint32>(FieldFlags::Transient) |
                                 static_cast<uint32>(FieldFlags::ReadOnly);
    return (parent & inherited) | own;
}

// 리플렉션 struct의 최대 중첩 깊이다(Docs/Design/ReflectionFoundations.ko.md 4.1·4.6절). 깊이는
// 1 + 멤버 struct 깊이의 최댓값(멤버 struct가 없으면 1)이다. MetaCompiler(RMC2303)·TypeRegistry·
// 생성 코드가 같은 값을 쓴다. codec 위치 프레임은 최대 1(최상위 필드) + 16개다.
inline constexpr uint32 MaxStructDepth = 16;

// 배열 필드 길이 상한(Docs/Design/ReflectionFoundations.ko.md 5.5·5.6절). DefaultMaxElements는
// MaxElements 지정자가 없을 때 MetaCompiler가 채우는 필드별 상한이고, MaxArrayElements는 필드별
// 상한(FieldSchema::maxElements)의 최댓값이자 로드가 배열마다 거는 유일한 원소 수 상한이다(로드는
// 필드별 maxElements를 걸지 않는다 — 상한을 낮춰도 기존 데이터가 열리고 다시 저장할 때 드러난다).
// MetaCompiler·TypeRegistry·codec이 같은 값을 쓴다.
inline constexpr uint32 DefaultMaxElements = 65536;
inline constexpr uint32 MaxArrayElements = 1048576;

enum class FieldKind : uint8 {
    Value,
    ObjectReference
};

// 저장/도구 경계에서 해석 가능한 필드 표현이다. Opaque는 기존 수동 메타데이터와의 호환 값이며
// typed accessor/직렬화를 제공한다는 뜻이 아니다. ObjectReference는 scalar byte 복사가 금지된다.
enum class ValueKind : uint8 {
    Opaque,
    Bool,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Int32,
    Int64,
    Float32,
    Float64,
    Guid,
    Utf8String,
    ObjectReference,
    // B3 이후의 값 종류다. Enum은 B3b, Struct는 B4, Array는 B5에서 등록을 열었다. Array 필드의
    // 원소 형식은 FieldSchema::element다(Array·Opaque는 원소가 될 수 없다).
    Enum,
    Struct,
    Array,
    AssetId,             // rocks::AssetId(Object/AssetId.h), wire Guid
    SoftObjectReference, // rocks::SoftObjectRef<T>(Object/SoftObjectRef.h), wire
                         // SoftObjectReference
    Digest256,           // rocks::Sha256Digest(Core/Encoding/Sha256.h), wire Bytes(정확히 32 byte)
    // 리플렉션 B9 이후의 값 종류다(Docs/Design/ReflectionExtensions.ko.md 1.1절).
    Int8,  // int8(Clang 철자 signed char — 평범한 char는 받지 않는다), wire I64(범위 검사)
    Int16, // int16(short), wire I64(범위 검사)
    Name,  // rocks::Name(Core/Text/Name.h — 실행 중 pool 토큰), wire String(표시 문자열 — None은
           // 빈 문자열). 복원은 1단계 수집 → 2a intern(Object/NameField.h)
    WeakObjectReference, // rocks::WeakObjectRef<T>(Object/ObjectRefs.h — GC가 추적하지 않는다),
                         // wire는 강한 참조와 같은 문맥 참조 wire(graph v2 U32 — 같은 그래프 안
                         // 노드만, Docs/Design/ReflectionExtensions.ko.md 5.1절)
    ClassReference,      // rocks::ClassRef<Base>(Object/ClassRef.h — 클래스 TypeId, 0 = null), wire
                         // Guid. value.type은 Base 클래스 ID(필수)다(같은 문서 5.2절)
    // rocks::Optional<T>(Core/Optional.h — 클래스 필드·struct 멤버만), wire Optional(present byte +
    // 내부 wire). value.type은 0이고 내부 형식은 FieldSchema::element다(같은 문서 6절 — 내부가 단일
    // 값이면 FieldValue로 읽고 쓰고, 값 struct면 aggregate).
    Optional,
    // rocks::OrderedMap<K, V>(Core/Containers/OrderedMap.h — 삽입 순서), wire Map(u32 키 wire + u32
    // 값 wire + count + (키, 값) × count). value.type은 0이고 키 형식은 FieldSchema::key, 값 형식은
    // FieldSchema::element다(같은 문서 7.2절 — aggregate, 필드 통째로 저장·복원한다).
    Map,
    // rocks::OrderedSet<T>(Core/Containers/OrderedSet.h — 삽입 순서), wire Set(u32 원소 wire +
    // count + 원소 × count). value.type은 0이고 원소 형식은 FieldSchema::element다(같은
    // 문서 7.2절).
    Set,
    // 정적 배열 T x[N](리플렉션 B9g — 같은 문서 8절), wire는 원소가 struct면 RecordList, 단일
    // 값이면 ValueArray(Array와 같다 — 늘 N개). value.type은 0이고 원소 형식은
    // FieldSchema::element, maxElements는 정확한 원소 수 N(1 이상 MaxStaticArrayElements 이하)이다.
    // aggregate이고 경로는 [i] 읽기·쓰기·멤버만 받는다(삽입·삭제·비우기는 Unsupported).
    StaticArray,
    // rocks::InstancedStruct<>(Object/InstancedStruct.h — 실행 중 정해지는 리플렉션 struct 값 하나
    // 또는 비어 있음, 리플렉션 B9j — 같은 문서 11절), wire InstancedStruct(필드 값은 빈 payload
    // 또는 담긴 struct의 Record, 원소·Map 값 자리는 u64 길이 + 같은 내용). value.type은 Base struct
    // ID(0 = 제한 없음 — B9j는 0만 받고 Base는 B9k가 연다)다. aggregate이고 경로는 형
    // 바꾸기·비우기와 지금 타입의 멤버 단계를 받는다.
    InstancedStruct,
    // rocks::Text(Localization/Text.h — 지역화 문구, 리플렉션 B9l — 같은 문서 13절), wire Text(형태
    // byte + 형태별 내용 — 비어 있음·Invariant·Localized만 저장한다). FieldValue로 통째 읽고 쓰며
    // (FieldValue::text — 읽기는 DeepClone), 키·Set 원소·Optional 내부가 아니고 Required를 받지
    // 않는다.
    Text
};

// 정적 배열 필드(ValueKind::StaticArray)의 원소 수 N의 상한이다(리플렉션 B9g —
// Docs/Design/ReflectionExtensions.ko.md 8절). UE 생성 코드의 uint16 ArrayDim과 같은 상한이다.
// MetaCompiler(RMC2307)·TypeRegistry·codec이 같은 값을 쓴다.
inline constexpr uint32 MaxStaticArrayElements = NumericLimits<uint16>::Max();

// ValueKind 열거자 수(마지막 열거자 + 1)다. 열거자 값은 schema·descriptor 계약이므로 새 종류는 끝에
// 붙이고, 이 식의 마지막 열거자와 성질 표의 행을 함께 고친다. 아래 static_assert가 성질 표와,
// MetaCompiler 대조 헤더가 C# 표의 종류 수와 이 값을 비교한다. 근거는
// Docs/Design/ReflectionExtensions.ko.md 1.3절이다.
inline constexpr uint32 ValueKindCount = static_cast<uint32>(ValueKind::Text) + 1;

// 값 종류의 성질이다(종류 성질 표 — Docs/Design/ReflectionExtensions.ko.md 1.3절).
// ValueKindProperties가 종류마다 이 비트의 OR를 돌려주고, 런타임의 등록 검증(TypeRegistry)·쓰기
// 검증·경로·생성 codec이 이 표로 종류를 판정한다. MetaCompiler는 같은 내용의 C#
// 표(ValueKindTable)로 판정하고, 대조 헤더가 두 표의 칸과 이 비트 값을 맞춘다(비트 값은 그 대조의
// 계약이며 저장 형식은 아니다).
//
// 위치 성질은 그 자리에 그 종류를 둘 수 있는지다. 컨테이너 모양과 struct 형질에 따른 제한은 표 밖의
// 규칙이다:
//   - ObjectReference의 Array 원소는 ObjectRefArray, Map 값은 ObjectRefMap(Registry 컨테이너)이다.
//   - Struct 행은 값 struct(강한 참조 없음) 기준이다. 참조 struct(ContainsStrongReference)는 Array
//     원소가 StructRefArray이고 Map 값·Optional 내부·InstancedStruct 내용이 될 수 없다.
//     InstancedStruct 내용은 Registry 컨테이너·InstancedStruct·Required도 담지 않아야 한다.
//   - 직접 중첩만 막는다. 컨테이너 종류는 다른 컨테이너의 원소·키·값·내부가 아니지만,
//     컨테이너 멤버를 담은 값 struct는 원소가 될 수 있다.
//   - aggregate 여부(WholeFieldValue)는 종류 단위다. 내부에 따라 달라지는 종류(Optional)는 필드
//     단위 판정(FieldIsAggregate)을 쓴다.
// Required·대상 타입 성질은 필드의 대상 종류(FieldTargetType — 컨테이너면 원소 종류)로 판정한다.
//
//   Field                  — 클래스 필드
//   StructMember           — 리플렉션 struct 멤버
//   ArrayElement           — Array 계열(Array·ObjectRefArray·StructRefArray) 원소
//   StaticArrayElement     — 정적 배열(T x[N]) 원소
//   MapKey·MapValue        — OrderedMap 키·값
//   SetElement             — OrderedSet 원소
//   OptionalInner          — Optional<T>의 T
//   InstancedStructContent — InstancedStruct가 담는 값
//   AcceptsRequired        — Required 지정자(그 값 또는 원소마다 non-null)
//   HasTargetType          — ValueType::type이 대상 타입 ID다(그 밖의 종류는 0이어야 한다)
//   RequiresTargetId       — 대상 ID가 0이면 안 된다(참조의 0은 Object라 허용한다)
//   Container              — 원소 형식(FieldSchema::element)을 갖는 컨테이너다
//   WholeFieldValue        — FieldValue로 통째 읽고 쓴다(아니면 aggregate — read·write가 없고 멤버·
//                            원소 단위 경로 연산으로만 쓴다)
// Optional 내부는 B9e, Map 키·값·Set 원소는 B9f2, Map 값의 ObjectReference(ObjectRefMap)는 B9f4,
// 정적 배열 원소는 B9g, InstancedStruct 내용과 InstancedStruct의 자리는 B9j가 열었다.
enum class ValueKindProperty : uint32 {
    None = 0,
    Field = 1u << 0,
    StructMember = 1u << 1,
    ArrayElement = 1u << 2,
    StaticArrayElement = 1u << 3,
    MapKey = 1u << 4,
    MapValue = 1u << 5,
    SetElement = 1u << 6,
    OptionalInner = 1u << 7,
    InstancedStructContent = 1u << 8,
    AcceptsRequired = 1u << 9,
    HasTargetType = 1u << 10,
    RequiresTargetId = 1u << 11,
    Container = 1u << 12,
    WholeFieldValue = 1u << 13
};

namespace detail {

constexpr uint32 ValueKindBit(ValueKindProperty property) noexcept {
    return static_cast<uint32>(property);
}

// 성질 표의 한 행이다: enumerator는 ValueKind 열거자인지(성질이 없는 Opaque 포함), properties는
// 성질 비트의 OR다.
struct ValueKindEntry {
    bool enumerator = false;
    uint32 properties = 0;
};

// 종류 성질의 유일한 표다(switch 하나 — default가 없어 열거자의 case를 빠뜨리면 -Wswitch 오류다).
constexpr ValueKindEntry ValueKindTableEntry(ValueKind kind) noexcept {
    // 단일 값의 자리 넷(클래스 필드·struct 멤버·Array 원소·정적 배열 원소)이다.
    constexpr uint32 placed = ValueKindBit(ValueKindProperty::Field) |
                              ValueKindBit(ValueKindProperty::StructMember) |
                              ValueKindBit(ValueKindProperty::ArrayElement) |
                              ValueKindBit(ValueKindProperty::StaticArrayElement);
    // Map 키·Set 원소·Optional 내부까지 되는 단일 값이다.
    constexpr uint32 keyed = placed | ValueKindBit(ValueKindProperty::MapKey) |
                             ValueKindBit(ValueKindProperty::MapValue) |
                             ValueKindBit(ValueKindProperty::SetElement) |
                             ValueKindBit(ValueKindProperty::OptionalInner) |
                             ValueKindBit(ValueKindProperty::WholeFieldValue);
    switch (kind) {
    case ValueKind::Opaque:
        return {true, 0};
    case ValueKind::Bool:
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Guid:
    case ValueKind::Utf8String:
    case ValueKind::Name:
    case ValueKind::AssetId:
    case ValueKind::Digest256:
        return {true, keyed};
    case ValueKind::Float32:
    case ValueKind::Float64:
        // 실수 키는 같음 규칙(NaN·-0)을 정할 소비자가 생길 때 연다.
        return {true, placed | ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::OptionalInner) |
                          ValueKindBit(ValueKindProperty::WholeFieldValue)};
    case ValueKind::Enum:
        return {true, keyed | ValueKindBit(ValueKindProperty::HasTargetType) |
                          ValueKindBit(ValueKindProperty::RequiresTargetId)};
    case ValueKind::ObjectReference:
        // 객체 정체성은 키가 아니고, null이 이미 "없음"이라 Optional 내부도 아니다.
        return {true, placed | ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::WholeFieldValue) |
                          ValueKindBit(ValueKindProperty::AcceptsRequired) |
                          ValueKindBit(ValueKindProperty::HasTargetType)};
    case ValueKind::WeakObjectReference:
        // 강한 참조와 같은 자리지만 Required는 받지 않는다 — 대상이 죽으면 저절로 null이 되므로
        // "늘 non-null"을 지킬 수 없다(UE weak 속성도 NonNullable이 될 수 없다).
        return {true, placed | ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::WholeFieldValue) |
                          ValueKindBit(ValueKindProperty::HasTargetType)};
    case ValueKind::ClassReference:
        // 클래스 TypeId 값이다. 키·원소가 될 수 있고 Required(non-null)를 받는다. 대상 ID는 Base
        // 클래스로 필수다(Object 전체를 받는 ClassRef는 없다). null이 이미 "없음"이라 Optional
        // 내부는 아니다.
        return {true, placed | ValueKindBit(ValueKindProperty::MapKey) |
                          ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::SetElement) |
                          ValueKindBit(ValueKindProperty::WholeFieldValue) |
                          ValueKindBit(ValueKindProperty::AcceptsRequired) |
                          ValueKindBit(ValueKindProperty::HasTargetType) |
                          ValueKindBit(ValueKindProperty::RequiresTargetId)};
    case ValueKind::SoftObjectReference:
        return {true, placed | ValueKindBit(ValueKindProperty::MapKey) |
                          ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::SetElement) |
                          ValueKindBit(ValueKindProperty::WholeFieldValue) |
                          ValueKindBit(ValueKindProperty::AcceptsRequired) |
                          ValueKindBit(ValueKindProperty::HasTargetType)};
    case ValueKind::Struct:
        return {true, placed | ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::OptionalInner) |
                          ValueKindBit(ValueKindProperty::InstancedStructContent) |
                          ValueKindBit(ValueKindProperty::HasTargetType) |
                          ValueKindBit(ValueKindProperty::RequiresTargetId)};
    case ValueKind::Array:
    case ValueKind::Optional:
    case ValueKind::Map:
    case ValueKind::Set:
    case ValueKind::StaticArray:
        // Optional은 내부 형식(element)을 갖는 컨테이너이고 필드·멤버 자리에만 둔다. 통째
        // FieldValue 여부는 내부에 따라 다르므로 이 칸은 비우고 FieldIsAggregate가 필드로 판정한다.
        // Map(값 형식 element, 키 형식 key)·Set(원소 형식 element)도 필드·멤버 자리의 컨테이너이고
        // aggregate다(B9f2는 필드 통째 저장·복원, key 단계 경로 연산은 B9f3). StaticArray(원소 형식
        // element, 원소 수 maxElements = N)도 같은 자리의 aggregate 컨테이너다(B9g).
        return {true, ValueKindBit(ValueKindProperty::Field) |
                          ValueKindBit(ValueKindProperty::StructMember) |
                          ValueKindBit(ValueKindProperty::Container)};
    case ValueKind::InstancedStruct:
        // 실행 중 정해지는 struct 값 하나다(리플렉션 B9j). 필드·멤버·Array·정적 배열 원소·Map 값
        // 자리에 두고 FieldValue로 통째 읽고 쓰지 않는다(aggregate — 형 바꾸기·비우기·멤버 경로).
        // 대상 type은 Base struct ID(0 = 제한 없음)다. 키·Set 원소·Optional 내부가 아니고(비어
        // 있음이 이미 "없음"이다) Required를 받지 않는다. InstancedStruct 내용도 아니다(중첩 금지 —
        // 담긴 struct가 InstancedStruct를 담지 않는다는 형질은 struct 형질로 따로 본다).
        return {true, placed | ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::HasTargetType)};
    case ValueKind::Text:
        // 지역화 문구 값이다(리플렉션 B9l). 같음이 문구 정체성이 아니라 wire byte라 키·Set 원소가
        // 아니고, 비어 있음이 이미 "없음"이라 Optional 내부도 Required도 아니다.
        return {true, placed | ValueKindBit(ValueKindProperty::MapValue) |
                          ValueKindBit(ValueKindProperty::WholeFieldValue)};
    }

    return {};
}

} // namespace detail

// ValueKindCount가 열거자 수와 같다: 마지막 값(ValueKindCount - 1)은 표의 행이고 ValueKindCount는
// 아니다. 새 열거자의 case를 더하고 ValueKindCount를 고치지 않으면 여기서 컴파일이 실패한다.
static_assert(detail::ValueKindTableEntry(static_cast<ValueKind>(ValueKindCount - 1)).enumerator &&
                  !detail::ValueKindTableEntry(static_cast<ValueKind>(ValueKindCount)).enumerator,
              "ValueKindCount must be the last ValueKind enumerator + 1");

namespace detail {

// Optional 내부 종류 가운데 Required를 받는 종류가 없는지다. Optional 필드는 unset이 정상 값이라
// Required를 받지 않는데, 등록·descriptor 검사는 Optional의 대상 종류(내부)로 Required를 판정하므로
// 이 불변식이 그 거부를 보장한다(리플렉션 B9e). 새 종류가 둘을 함께 가지면 아래 static_assert가
// 실패한다 — 그때는 Optional 필드의 Required 거부를 따로 둔다.
constexpr bool NoOptionalInnerAcceptsRequired() noexcept {
    const uint32 both = ValueKindBit(ValueKindProperty::OptionalInner) |
                        ValueKindBit(ValueKindProperty::AcceptsRequired);
    for (uint32 value = 0; value < ValueKindCount; ++value) {
        if ((ValueKindTableEntry(static_cast<ValueKind>(value)).properties & both) == both) {
            return false;
        }
    }

    return true;
}

} // namespace detail

static_assert(
    detail::NoOptionalInnerAcceptsRequired(),
    "an Optional inner value kind must not accept Required (Optional fields reject Required)");

// 값 종류의 성질 비트(ValueKindProperty의 OR)다. 아래 질의 함수가 모두 이것을 읽는다.
// Opaque(비typed 수동 메타데이터)와 열거자 밖의 값은 0이다.
//
// 사용 예:
//   if (ValueKindCanBeArrayElement(field.element.kind)) { ... }   // 보통은 이름 있는 질의를 쓴다
//   const uint32 bits = ValueKindProperties(kind);                  // 여러 성질을 한 번에 볼 때
constexpr uint32 ValueKindProperties(ValueKind kind) noexcept {
    return detail::ValueKindTableEntry(kind).properties;
}

// kind가 성질 property 하나를 가졌는지다(ValueKindProperties 조회). property는 성질 하나여야 한다 —
// None이나 여러 비트의 OR는 계약 위반이다(ROCKS_ASSERT, 상수 식이면 컴파일 오류). C#
// ValueKindTable.Has와 같은 계약이다. 열거자 밖의 값은 어느 성질도 없다.
constexpr bool HasValueKindProperty(ValueKind kind, ValueKindProperty property) noexcept {
    const uint32 bit = static_cast<uint32>(property);
    ROCKS_ASSERT(bit != 0 && (bit & (bit - 1)) == 0);
    return (ValueKindProperties(kind) & bit) != 0;
}

// 클래스 필드의 값 종류가 될 수 있는지다(Opaque는 표 밖 — 등록이 비typed 필드로 따로 받는다).
constexpr bool ValueKindCanBeField(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::Field);
}

// 리플렉션 struct 멤버의 값 종류가 될 수 있는지다.
constexpr bool ValueKindCanBeStructMember(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::StructMember);
}

// Array 계열 원소가 될 수 있는지다(ObjectReference는 ObjectRefArray, 참조 struct는 StructRefArray).
constexpr bool ValueKindCanBeArrayElement(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::ArrayElement);
}

// 정적 배열 원소가 될 수 있는지다.
constexpr bool ValueKindCanBeStaticArrayElement(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::StaticArrayElement);
}

// OrderedMap 키가 될 수 있는지다.
constexpr bool ValueKindCanBeMapKey(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::MapKey);
}

// OrderedMap 값이 될 수 있는지다(ObjectReference는 ObjectRefMap, 참조 struct는 아니다).
constexpr bool ValueKindCanBeMapValue(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::MapValue);
}

// OrderedSet 원소가 될 수 있는지다.
constexpr bool ValueKindCanBeSetElement(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::SetElement);
}

// Optional<T>의 T가 될 수 있는지다(참조 struct는 아니다).
constexpr bool ValueKindCanBeOptionalInner(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::OptionalInner);
}

// InstancedStruct가 담는 값이 될 수 있는지다(담을 수 있는 struct 조건은 struct 형질로 따로 본다).
constexpr bool ValueKindCanBeInstancedStructContent(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::InstancedStructContent);
}

// 대상 종류에 Required를 쓸 수 있는지다.
constexpr bool ValueKindAcceptsRequired(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::AcceptsRequired);
}

// 대상 종류의 ValueType::type이 대상 타입 ID인지다(아니면 type은 0이어야 한다).
constexpr bool ValueKindHasTargetType(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::HasTargetType);
}

// 대상 종류의 대상 ID가 0이면 안 되는지다.
constexpr bool ValueKindRequiresTargetId(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::RequiresTargetId);
}

// 원소 형식(FieldSchema::element)을 갖는 컨테이너 종류인지다.
constexpr bool ValueKindIsContainer(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::Container);
}

// FieldValue로 통째 읽고 쓰는 종류인지다.
constexpr bool ValueKindIsWholeFieldValue(ValueKind kind) noexcept {
    return HasValueKindProperty(kind, ValueKindProperty::WholeFieldValue);
}

// typed 필드 값 종류 가운데 FieldValue로 통째 읽고 쓰지 않는 aggregate(Struct·Array·Optional·
// Map·Set·StaticArray·InstancedStruct)인지다(클래스 필드가 될 수 있고 WholeFieldValue가 아니다 —
// 표를 한 번 읽는다). aggregate 필드는 read·write·writeStaged가 없고 멤버·원소 단위 경로 연산으로만
// 쓴다. Opaque와 열거자 밖의 값은 aggregate가 아니다. 종류 단위 판정이라 Optional은 늘 참이다 —
// 필드는 내부에 따라 달라지므로 필드 하나를 판정할 때는 FieldIsAggregate를 쓴다(Optional<단일 값>은
// aggregate가 아니다).
constexpr bool ValueKindIsAggregate(ValueKind kind) noexcept {
    const uint32 properties = ValueKindProperties(kind);
    return (properties & detail::ValueKindBit(ValueKindProperty::Field)) != 0 &&
           (properties & detail::ValueKindBit(ValueKindProperty::WholeFieldValue)) == 0;
}

// 필드 값 형식이다. type은 Enum·Struct 스키마 ID, ObjectReference·WeakObjectReference·
// SoftObjectReference의 기대 대상 타입(0 = Object), ClassReference의 Base 클래스 ID(0이 아니어야
// 한다), InstancedStruct의 Base struct ID(0 = 제한 없음)에만 쓰고 그 밖은 0이어야 한다(Array의
// value.type도 0 — 원소 대상은 element.type).
struct ValueType {
    ValueKind kind = ValueKind::Opaque;
    TypeId type{};
};

// 등록 입력은 비소유 뷰다. 이름은 길이 기반이며 널 종료를 요구하지 않는다.
// TypeRegistry::Register는 필드·이름·속성 바이트를 깊게 복사한다.
// FieldKind만으로 GC 간선을 자동 발견하지 않으며, 현재 Trace 구현이 필요하다.
struct FieldSchema {
    Guid id;
    FieldKind kind = FieldKind::Value;
    uint32 flags = 0;
    Span<const char> name;
    Span<const uint8> attributes;
    // 값 형식이다. typed 필드(value.kind ≠ Opaque)에서 FieldKind::ObjectReference ⇔ value.kind가
    // ObjectReference(또는 원소가 ObjectReference인 Array, 값이 ObjectReference인 Map —
    // ObjectRefMap)다. 접근기 없는 Opaque 필드는 수동
    // Trace의 간선을 FieldKind::ObjectReference로 서술할 수 있다.
    ValueType value{};
    // 표시 문자열도 등록 시 깊게 복사한다. category는 그룹, displayName은 표시용이며 ID가 아니다.
    Span<const char> category{};
    Span<const char> displayName{};
    // 도구·명령행 이름(ROCKS_PROPERTY(Name="…"))이다. 비어 있으면 name을 쓴다. 비어 있지 않으면
    // ASCII 식별자([A-Za-z_][A-Za-z0-9_]*)여야 하고, 실효 이름(toolName 또는 name)은 상속 필드를
    // 포함한 타입의 필드 사이에서 대소문자 무시로 유일해야 한다. 직렬화는 필드 ID를 쓰므로 이름과
    // 무관하다.
    Span<const char> toolName{};
    // Array 필드(value = {Array, 0})의 원소 형식과 길이 상한이다(B5 — maxElements는 1 이상
    // MaxArrayElements 이하, 저장·경로 쓰기에서 건다). 원소는 스칼라(Int8·Int16 포함), Guid,
    // Utf8String, Name, ObjectReference, WeakObjectReference, ClassReference, Enum, Struct,
    // AssetId, SoftObjectReference, Digest256이고 element.type은 Enum·Struct·ClassReference면 필수,
    // 세 참조는 기대 타입(선택), 그 밖은 0이다. Optional 필드(value = {Optional, 0} — 리플렉션
    // B9e)는 element가 내부 형식(성질 표의 Optional 내부 종류 — 단일 값·String·Name·값 struct)이고
    // maxElements는 0이다. Map 필드(value = {Map, 0} — 리플렉션 B9f2)는 element가 값 형식(성질 표의
    // Map 값 종류 — ObjectReference는 ObjectRefMap(리플렉션 B9f4), 값 struct만), Set 필드(value =
    // {Set, 0})는 element가 원소 형식(성질 표의 Set 원소 종류)이고 둘 다 maxElements는 1 이상
    // MaxArrayElements 이하의 원소 수 상한이다(저장에서만 건다 — 배열과 같다). StaticArray 필드
    // (value = {StaticArray, 0} — 리플렉션 B9g)는 element가 원소 형식(성질 표의 정적 배열 원소 종류
    // — ObjectReference면 FieldKind::ObjectReference)이고 maxElements는 정확한 원소 수 N(1 이상
    // MaxStaticArrayElements 이하)이다. 그 밖의 필드는 Opaque·0이어야 한다.
    ValueType element{};
    uint32 maxElements = 0;
    // Map 필드의 키 형식이다(리플렉션 B9f2 — Docs/Design/ReflectionExtensions.ko.md 1.1·7.2절). 키
    // 종류는 성질 표의 Map 키 종류(정수 8종·Bool·Enum·Guid·AssetId·Digest256·String·Name·
    // SoftObjectReference·ClassReference)이고 key.type은 Enum이면 enum ID(필수), ClassReference면
    // Base 클래스 ID(필수), SoftObjectReference면 기대 타입(0 = Object), 그 밖은 0이다. 키에는
    // Required가 없다(필드의 Required는 값마다 non-null — 대상 형식 element로 판정한다). Map이 아닌
    // 필드는 {Opaque, 0}이어야 한다.
    ValueType key{};
    // 에셋 bundle 이름 목록(ROCKS_PROPERTY(AssetBundles="a,b") —
    // Docs/Design/AssetSystem 10.2절 A4e2a, UE meta=(AssetBundles=…))이다. 쉼표로 이은 정규
    // 텍스트(공백 없음 — IsAssetBundleList)이고 비면 없다. 대상 종류(FieldTargetType)가
    // SoftObjectReference·Struct· InstancedStruct이거나 Map 키가 SoftObjectReference인 필드에만
    // 쓰고, EditorOnly 필드에는 쓰지 않는다(등록 InvalidArgument). 저장 때 bundle 태그만 읽는 편집
    // 정보라 UiToolkit 7.3 편집 메타데이터 블록(CU1)이 생기면 그리로 옮긴다.
    Span<const char> assetBundles{};
};

// bundle 이름 하나의 byte 상한과 목록의 이름 수 상한이다(AssetSystem 10.2절 A4e2a).
inline constexpr usize AssetBundleNameMaxBytes = 64;
inline constexpr usize AssetBundleMaxNames = 16;

// list가 정규 bundle 목록인지다: 쉼표로 이은 이름 1–AssetBundleMaxNames개, 이름마다 ASCII
// 식별자([A-Za-z_][A-Za-z0-9_]*, 1–AssetBundleNameMaxBytes byte), 목록 안 중복 없음(대소문자
// 구분 — 태그 키가 `Bundle.<이름>`이다), 공백 없음.
constexpr bool IsAssetBundleList(Span<const char> list) noexcept {
    usize names = 0;
    usize start = 0;
    for (usize end = 0; end <= list.Size(); ++end) {
        if (end < list.Size() && list[end] != ',') {
            const char c = list[end];
            const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
            if (!letter && (end == start || c < '0' || c > '9')) {
                return false;
            }

            continue;
        }

        const usize length = end - start;
        if (length == 0 || length > AssetBundleNameMaxBytes || ++names > AssetBundleMaxNames) {
            return false;
        }

        // 앞선 이름과 같은지 본다(목록이 짧아 제곱 비교로 충분하다).
        usize other = 0;
        while (other < start) {
            usize otherEnd = other;
            while (list[otherEnd] != ',') {
                ++otherEnd;
            }

            bool same = otherEnd - other == length;
            for (usize at = 0; same && at < length; ++at) {
                same = list[other + at] == list[start + at];
            }

            if (same) {
                return false;
            }

            other = otherEnd + 1;
        }

        start = end + 1;
    }

    return true;
}

// 경로의 index 단계([i])를 받는 컨테이너 종류(Array·StaticArray)인지다. 원소 수를 바꾸는
// 삽입·삭제·비우기는 Array만 받는다(StaticArray는 길이가 N으로 고정 — 리플렉션 B9g).
//
// 사용 예:
//   if (step.selector == PathSelector::Index && !ValueKindTakesIndex(field.value.kind)) { ... }
constexpr bool ValueKindTakesIndex(ValueKind kind) noexcept {
    return kind == ValueKind::Array || kind == ValueKind::StaticArray;
}

// 필드 값의 대상 형식이다: 컨테이너 종류(ValueKindIsContainer — Array·Optional·Map·Set·StaticArray)
// 필드면 원소·내부·값 형식(element), 아니면 값 형식(value). enum·struct·참조 대상
// 해석(TypeRegistry의 의존·FieldEnum·FieldStruct, codec 검증, 그래프 기대 타입)이 같은 규칙을 쓴다.
// Map 키는 FieldKeyType이다. 반환 참조는 field의 수명을 따른다.
//
// 사용 예:
//   const ValueType& target = FieldTargetType(field);
//   if (target.kind == ValueKind::Enum) { ... }   // Enum 필드이거나 enum 원소 배열·Optional<enum>
constexpr const ValueType& FieldTargetType(const FieldSchema& field) noexcept {
    return ValueKindIsContainer(field.value.kind) ? field.element : field.value;
}

// Map 필드의 키 형식이다(리플렉션 B9f2 — 그 밖의 필드는 {Opaque, 0}인 key 칸 그대로). 키 대상(Enum
// 레코드·ClassRef Base·Soft 기대 타입)은 TypeRegistry가 이 형식으로
// 해석한다(TypeLease::FieldKeyEnum). 반환 참조는 field의 수명을 따른다.
//
// 사용 예:
//   if (FieldKeyType(field).kind == ValueKind::Enum) { ... }   // enum 키 Map
constexpr const ValueType& FieldKeyType(const FieldSchema& field) noexcept {
    return field.key;
}

// 필드가 FieldValue로 통째 읽고 쓰지 않는 aggregate인지다(필드 단위 판정 — Docs/Design/
// ReflectionExtensions.ko.md 1.3절). Optional은 내부가 통째 FieldValue 종류(단일 값·String·Name)면
// aggregate가 아니고(FieldValue::kind = 내부 종류, present = set 여부로 읽고 쓴다) 값 struct면
// aggregate다. 그 밖은 ValueKindIsAggregate(value.kind)다. TypeRegistry 등록(read·write 유무)·
// FieldWriteOperation·경로 끝 분류가 이 판정을 쓴다.
//
// 사용 예:
//   if (FieldIsAggregate(field)) { return Result<void>::Failure(ErrorCode::Unsupported); }
constexpr bool FieldIsAggregate(const FieldSchema& field) noexcept {
    if (field.value.kind == ValueKind::Optional) {
        return !ValueKindIsWholeFieldValue(field.element.kind);
    }

    return ValueKindIsAggregate(field.value.kind);
}

// 안정적인 타입 식별자인 id와 스키마 version을 실행 중 native type key와 구분한다.
// 등록된 스키마는 불변이다. 변경하려면 기존 lease를 정리하고 재등록해야 한다.
struct TypeSchema {
    TypeId id;
    uint32 version = 1;
    Span<const FieldSchema> fields;
    Span<const char> name;
    // 선언된 단일 base schema의 ID다. 0은 reflected base가 없는 타입(Object 직접 파생)이다.
    TypeId baseType{};
    uint32 flags = 0;
    // 타입 도구 이름이다(클래스·struct — Docs/Design/ReflectionExtensions.ko.md 5.2절). 에셋 명령의
    // `<Type>`(asset-new·asset-list --type)·ClassRef 텍스트·InstancedStruct `_structType`·Sparks가
    // 이 이름 하나로 타입을 가리킨다. MetaCompiler는 늘 채운다(기본은 한정하지 않은 C++ 타입 이름,
    // ROCKS_CLASS(Name="…")·ROCKS_STRUCT(Name="…")로 바꾼다). 비어 있으면 도구 이름이 없는 타입이다
    // (수동 등록 — TypeRegistry::AcquireByToolName으로 찾을 수 없다). 비어 있지 않으면
    // IsTypeToolName을 만족해야 하고 TypeRegistry 안에서 대소문자 무시로 유일하다(등록
    // AlreadyExists). 그래서 생성 스키마를 복사해 같은 C++ 타입을 다른 TypeId로 등록하는 별칭
    // 스키마는 이 칸을 비운다(원본과 이름이 같아 AlreadyExists). 등록이 깊게 복사한다. 직렬화는
    // TypeId를 쓰므로 이 이름과 무관하다.
    Span<const char> toolName{};
    // Within 클래스 ID다(클래스만, 0 = 제한 없음 — 리플렉션 B9h2,
    // Docs/Design/ReflectionExtensions.ko.md 9.2절). 0이 아니면 이 클래스 객체의 Outer가 그 클래스
    // 또는 파생이어야 하고 Outer 없는 객체는 될 수 없다(생성 정체성 규칙 — InvalidArgument). 파생은
    // base의 Within을 상속한다(MetaCompiler가 파생 스키마에 그대로 싣는다 — 다시 지정하면 base
    // Within의 파생이어야 한다, RMC2208). TypeRegistry는 base에 Within이 있는데 파생이 0이면
    // InvalidArgument다. struct는 0이어야 한다.
    TypeId withinType{};
    // 이 타입(상속 포함)의 EditorOnly 필드 ID 목록이다(Docs/Design/BuildTargets 3.4절 BS2c). 빌드
    // 조합과 무관하게 같은 값이다: ROCKS_WITH_EDITOR_DATA=1이면 그 필드가 fields에도 있고(그때는
    // EditorOnly여야 한다), 0이면 fields에 없다(필드가 컴파일에서 빠진다). 에디터 데이터 없는
    // 빌드가 에디터 패키지를 읽을 때 이 ID의 저장 값을 진단 없이 건너뛴다. 중복·0 ID와 EditorOnly가
    // 아닌 필드의 ID는 InvalidArgument다. 등록이 깊게 복사한다. MetaCompiler가 채우고, 수동 등록은
    // 비워 둘 수 있다(리플렉션 조상의 필드를 빌리는 native 클래스는 조상의 목록을 물려받는다).
    Span<const Guid> editorOnlyFieldIds{};
};

// schema가 id를 EditorOnly 필드 ID로 싣는지다(editorOnlyFieldIds — 선형 탐색, 목록은 작다).
//
// 사용 예:
//   if (IsEditorOnlyFieldId(schema, storedId)) { /* 에디터 데이터 없는 빌드: 값을 건너뛴다 */ }
constexpr bool IsEditorOnlyFieldId(const TypeSchema& schema, Guid id) noexcept {
    for (usize index = 0; index < schema.editorOnlyFieldIds.Size(); ++index) {
        if (schema.editorOnlyFieldIds[index] == id) {
            return true;
        }
    }

    return false;
}

// 타입 도구 이름의 문법이다: `[A-Za-z_][A-Za-z0-9_]*('.' [A-Za-z_][A-Za-z0-9_]*)*`(Sparks 모듈 경로
// `Weapons.Sword` — `:`는 쓰지 않는다). `none`은 ClassRef·참조 텍스트의 null 표기라 대소문자 무시로
// 예약이다(Docs/Design/ReflectionExtensions.ko.md 5.2·10절). 빈 이름은 거짓이다. TypeRegistry
// 등록과 MetaCompiler(RMC2203 — C# TypeToolName.IsValid)가 같은 규칙을 쓴다.
//
// 사용 예:
//   if (schema.toolName.Size() != 0 && !IsTypeToolName(schema.toolName)) { ... }
constexpr bool IsTypeToolName(Span<const char> name) noexcept {
    bool start = true;
    for (usize index = 0; index < name.Size(); ++index) {
        const char c = name[index];
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (c == '.') {
            if (start) {
                return false;
            }

            start = true;
            continue;
        }

        if (!letter && (start || c < '0' || c > '9')) {
            return false;
        }

        start = false;
    }

    constexpr char reserved[] = {'n', 'o', 'n', 'e'};
    bool none = name.Size() == sizeof(reserved);
    for (usize index = 0; none && index < sizeof(reserved); ++index) {
        none = (name[index] >= 'A' && name[index] <= 'Z' ? name[index] - 'A' + 'a' : name[index]) ==
               reserved[index];
    }

    return !start && !none;
}

// EnumFlags:
//   Flags — 비트 플래그 enum이다(ROCKS_ENUM(Flags)). 값은 선언 값들의 OR(0 포함)이면 유효하다.
//           열거자 값은 음수일 수 없다(uint64 Flags enum은 비트 0–62 — 값이 int64이기 때문이다).
enum class EnumFlags : uint32 {
    None = 0,
    Flags = 1
};

// EnumeratorFlags:
//   Hidden — 저장 값으로는 유효하지만 도구가 설정 값으로 받지 않는 열거자다(UE UMETA(Hidden) 대응,
//            판정은 설정 계층).
enum class EnumeratorFlags : uint32 {
    None = 0,
    Hidden = 1
};

constexpr bool HasFlag(uint32 flags, EnumFlags flag) noexcept {
    return (flags & static_cast<uint32>(flag)) != 0;
}

constexpr bool HasFlag(uint32 flags, EnumeratorFlags flag) noexcept {
    return (flags & static_cast<uint32>(flag)) != 0;
}

// enum 열거자 하나다(Docs/Design/ReflectionFoundations.ko.md 3.4절). 이름은 길이 기반 비소유 뷰이며
// 등록 때 깊게 복사한다.
struct EnumeratorSchema {
    // C++ 열거자 이름(ASCII 식별자)이다.
    Span<const char> name;
    // 도구 이름(ROCKS_ENUMERATOR(Name="…"))이다. 비면 name을 쓴다. [A-Za-z0-9_]+이며 숫자로 시작할
    // 수 있다(접두를 뗀 UE 이름 "8BitData"). ToolPrefix는 포함하지 않는다.
    Span<const char> toolName{};
    // 표시용 UTF-8 문자열이다(UE UMETA(DisplayName)). ID가 아니다.
    Span<const char> displayName{};
    int64 value = 0;
    uint32 flags = 0;
};

// 리플렉션 enum의 스키마다(ROCKS_ENUM). 필드에서는 ValueType{Enum, id}로 가리키고 저장 wire는
// I64다. underlying은 UE와 같은 정수 8종(UInt8·UInt16·UInt32·UInt64·Int8·Int16·Int32·Int64 —
// EnumUnderlyingRange) 중 하나이며 열거자 값은 그 범위 안에서 서로 다르다(별칭 금지). 값이
// int64라서 UInt64 enum의 열거자 값은 int64 최댓값 이하다. 등록은 TypeRegistry::RegisterEnum이다.
struct EnumSchema {
    TypeId id;
    Span<const char> name;
    ValueKind underlying = ValueKind::Opaque;
    uint32 flags = 0;
    Span<const EnumeratorSchema> values;
    // 도구 이름 조회가 앞에 붙어 있어도 받는 접두(ROCKS_ENUM(ToolPrefix="…"), 예:
    // "TEXTUREGROUP_")다.
    Span<const char> toolPrefix{};
};

// underlying 폭의 값 범위를 minimum·maximum에 쓴다. underlying은 UE와 같은 정수 8종이고 그 밖의
// 종류면 false다(minimum·maximum은 그대로다). 근거는 Docs/Design/ReflectionExtensions.ko.md
// 2절이다. 저장 값이 int64이므로 UInt64의 최댓값은 int64 최댓값이다(MetaCompiler가 그보다 큰
// 열거자를 RMC2401로 거부한다 — UE UEnum도 값을 int64로 두지만 넘는 값을 음수로 감싸 받는다,
// 같은 문서 18.1절). MetaCompiler EnumRules.Range와 같은 표다.
//
// 사용 예:
//   int64 minimum = 0;
//   int64 maximum = 0;
//   if (!EnumUnderlyingRange(schema.underlying, minimum, maximum)) { return InvalidArgument; }
constexpr bool EnumUnderlyingRange(ValueKind underlying, int64& minimum, int64& maximum) noexcept {
    switch (underlying) {
    case ValueKind::UInt8:
        minimum = 0;
        maximum = NumericLimits<uint8>::Max();
        return true;
    case ValueKind::UInt16:
        minimum = 0;
        maximum = NumericLimits<uint16>::Max();
        return true;
    case ValueKind::UInt32:
        minimum = 0;
        maximum = NumericLimits<uint32>::Max();
        return true;
    case ValueKind::UInt64:
        minimum = 0;
        maximum = NumericLimits<int64>::Max();
        return true;
    case ValueKind::Int8:
        minimum = NumericLimits<int8>::Min();
        maximum = NumericLimits<int8>::Max();
        return true;
    case ValueKind::Int16:
        minimum = NumericLimits<int16>::Min();
        maximum = NumericLimits<int16>::Max();
        return true;
    case ValueKind::Int32:
        minimum = NumericLimits<int32>::Min();
        maximum = NumericLimits<int32>::Max();
        return true;
    case ValueKind::Int64:
        minimum = NumericLimits<int64>::Min();
        maximum = NumericLimits<int64>::Max();
        return true;
    default:
        return false;
    }
}

// value가 이 enum의 유효한 저장 값인지다. 일반 enum은 선언된 값이어야 하고, Flags enum은 선언
// 값들의 OR(0 포함)이어야 한다. 어느 쪽이든 underlying 범위 안이어야 한다. 쓰기·저장·복원의 검증과
// 같은 규칙이다(생성 헤더의 GeneratedType<E>::IsDeclared도 같다).
constexpr bool IsDeclaredEnumValue(const EnumSchema& schema, int64 value) noexcept {
    int64 minimum = 0;
    int64 maximum = 0;
    if (!EnumUnderlyingRange(schema.underlying, minimum, maximum) || value < minimum ||
        value > maximum) {
        return false;
    }

    if (HasFlag(schema.flags, EnumFlags::Flags)) {
        uint64 mask = 0;
        for (usize index = 0; index < schema.values.Size(); ++index) {
            mask |= static_cast<uint64>(schema.values[index].value);
        }

        return value >= 0 && (static_cast<uint64>(value) & ~mask) == 0;
    }

    for (usize index = 0; index < schema.values.Size(); ++index) {
        if (schema.values[index].value == value) {
            return true;
        }
    }

    return false;
}

// 선언 값들의 OR(Flags enum의 유효 비트)다.
constexpr uint64 EnumDeclaredMask(const EnumSchema& schema) noexcept {
    uint64 mask = 0;
    for (usize index = 0; index < schema.values.Size(); ++index) {
        mask |= static_cast<uint64>(schema.values[index].value);
    }

    return mask;
}

// 저장된 미선언 값을 대신할 값이다(decode 정책 ReportAndDefault, 6.4절). 일반 enum은 값 0이
// 선언되어 있으면 0, 아니면 첫 열거자다. Flags enum은 underlying 범위 안의 값이면 선언되지 않은
// 비트만 지운 값이고(알려진 비트를 잃지 않는다), 범위 밖·음수 값이면 0이다(-1이 모든 플래그를 켜지
// 않게). 결과는 항상 IsDeclaredEnumValue를 만족한다. 열거자가 없는 스키마는 0이다(등록이 막는다).
constexpr int64 EnumReplacementValue(const EnumSchema& schema, int64 stored) noexcept {
    if (HasFlag(schema.flags, EnumFlags::Flags)) {
        int64 minimum = 0;
        int64 maximum = 0;
        if (!EnumUnderlyingRange(schema.underlying, minimum, maximum) || stored < 0 ||
            stored > maximum) {
            return 0;
        }

        return static_cast<int64>(static_cast<uint64>(stored) & EnumDeclaredMask(schema));
    }

    for (usize index = 0; index < schema.values.Size(); ++index) {
        if (schema.values[index].value == 0) {
            return 0;
        }
    }

    return schema.values.Size() != 0 ? schema.values[0].value : 0;
}

namespace detail {

// ASCII 대문자만 소문자로 바꾼다(그 밖의 byte는 그대로다).
constexpr char AsciiLower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// left가 right[offset..]과 ASCII 대소문자 무시로 같은지다(도구 이름 조회 — enum 열거자·필드·경로가
// 같이 쓴다).
constexpr bool EnumNameEquals(Span<const char> left, Span<const char> right,
                              usize offset = 0) noexcept {
    if (offset > right.Size() || left.Size() != right.Size() - offset) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[offset + index])) {
            return false;
        }
    }

    return true;
}

} // namespace detail

// 두 이름이 ASCII 대소문자 무시로 같은지다(ASCII가 아닌 byte는 그대로 비교한다). 필드·열거자 도구
// 이름의 유일성 검사(TypeRegistry)와 경로 이름 조회(ParsePropertyPath)가 같은 규칙을 쓴다.
//
// 사용 예:
//   if (EqualsIgnoringAsciiCase(EffectiveToolName(field), name)) { ... }
constexpr bool EqualsIgnoringAsciiCase(Span<const char> left, Span<const char> right) noexcept {
    return detail::EnumNameEquals(left, right);
}

// 열거자의 실효 도구 이름이다(toolName이 비면 C++ 이름).
constexpr Span<const char> EffectiveToolName(const EnumeratorSchema& value) noexcept {
    return value.toolName.Size() != 0 ? value.toolName : value.name;
}

// 필드의 실효 도구 이름이다(toolName이 비면 C++ 이름). toolName이 있으면 도구·경로는 C++ 이름을
// 받지 않는다(AS 13절의 "Name으로 노출한 이름"). 반환 뷰는 field의 수명을 따른다.
constexpr Span<const char> EffectiveToolName(const FieldSchema& field) noexcept {
    return field.toolName.Size() != 0 ? field.toolName : field.name;
}

// 도구 이름으로 열거자를 찾는다. ASCII 대소문자를 무시하고, toolPrefix가 있으면 접두가 붙은
// 이름("TEXTUREGROUP_8BitData")과 뗀 이름("8BitData")을 모두 받는다. 없으면 nullptr이다. 반환
// 포인터는 schema의 수명을 따른다. Hidden 열거자도 찾는다(설정 값으로 받을지는 호출자가 flags로
// 판단한다).
constexpr const EnumeratorSchema* FindEnumerator(const EnumSchema& schema,
                                                 Span<const char> toolName) noexcept {
    const usize prefix = schema.toolPrefix.Size();
    bool prefixed = prefix != 0 && toolName.Size() >= prefix;
    for (usize index = 0; prefixed && index < prefix; ++index) {
        prefixed =
            detail::AsciiLower(toolName[index]) == detail::AsciiLower(schema.toolPrefix[index]);
    }

    for (usize index = 0; index < schema.values.Size(); ++index) {
        const Span<const char> name = EffectiveToolName(schema.values[index]);
        if (detail::EnumNameEquals(name, toolName) ||
            (prefixed && detail::EnumNameEquals(name, toolName, prefix))) {
            return &schema.values[index];
        }
    }

    return nullptr;
}

} // namespace rocks
