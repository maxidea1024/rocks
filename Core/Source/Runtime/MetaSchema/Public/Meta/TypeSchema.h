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
enum class TypeFlags : uint32 {
    None = 0,
    Abstract = 1,
    WorldOnly = 2,
    Atomic = 16
};

// FieldFlags:
//   Transient  — GC 추적은 하되 저장 그래프에서 뺀다.
//   ReadOnly   — 일반 write를 거부하고 생성·복원의 staged write만 허용한다.
//   EditorOnly — 항상 컴파일되고 플래그만 단다. 쿠킹 데이터에서 빼는 것은 graph v2(A0-O)다.
//   Required   — typed 참조·소프트 참조 필드와 그 원소 배열(ObjectRefArray,
//                Array<SoftObjectRef>)에만 쓴다. 배열이면 원소마다 non-null이라는
//                뜻이다(비어 있지 않아야 한다는 뜻은 없다). null 거부는 패키지
//                저장·쿠킹(A1)이고 로드는 진단만 한다.
enum class FieldFlags : uint32 {
    None = 0,
    Transient = 1,
    ReadOnly = 2,
    EditorOnly = 4,
    Required = 8
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
    WeakObjectReference, // rocks::WeakRef<T>(Object/ObjectRefs.h — GC가 추적하지 않는다), wire는
                         // 강한 참조와 같은 문맥 참조 wire(graph v2 U32 — 같은 그래프 안 노드만,
                         // Docs/Design/ReflectionExtensions.ko.md 5.1절)
    ClassReference,      // rocks::ClassRef<Base>(Object/ClassRef.h — 클래스 TypeId, 0 = null), wire
                         // Guid. value.type은 Base 클래스 ID(필수)다(같은 문서 5.2절)
    // rocks::Optional<T>(Core/Optional.h — 클래스 필드·struct 멤버만), wire Optional(present byte +
    // 내부 wire). value.type은 0이고 내부 형식은 FieldSchema::element다(같은 문서 6절 — 내부가 단일
    // 값이면 FieldValue로 읽고 쓰고, 값 struct면 aggregate).
    Optional
};

// ValueKind 열거자 수(마지막 열거자 + 1)다. 열거자 값은 schema·descriptor 계약이므로 새 종류는 끝에
// 붙이고, 이 식의 마지막 열거자와 성질 표의 행을 함께 고친다. 아래 static_assert가 성질 표와,
// MetaCompiler 대조 헤더가 C# 표의 종류 수와 이 값을 비교한다. 근거는
// Docs/Design/ReflectionExtensions.ko.md 1.3절이다.
inline constexpr uint32 ValueKindCount = static_cast<uint32>(ValueKind::Optional) + 1;

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
// StaticArray·Map·Set·InstancedStruct 자리는 리플렉션 B9 뒤 단계가 연다(Optional 내부는 B9e가
// 열었다). 이 표는 그 자리의 허용 여부를 미리 정해 두며, 그 단계 전에는 쓰는 곳이 없다.
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
        // Optional은 내부 형식(element)을 갖는 컨테이너이고 필드·멤버 자리에만 둔다. 통째
        // FieldValue 여부는 내부에 따라 다르므로 이 칸은 비우고 FieldIsAggregate가 필드로 판정한다.
        return {true, ValueKindBit(ValueKindProperty::Field) |
                          ValueKindBit(ValueKindProperty::StructMember) |
                          ValueKindBit(ValueKindProperty::Container)};
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

// typed 필드 값 종류 가운데 FieldValue로 통째 읽고 쓰지 않는 aggregate(Struct·Array·Optional)인지다
// (클래스 필드가 될 수 있고 WholeFieldValue가 아니다 — 표를 한 번 읽는다). aggregate 필드는 read·
// write·writeStaged가 없고 멤버·원소 단위 경로 연산으로만 쓴다. Opaque와 열거자 밖의 값은
// aggregate가 아니다. 종류 단위 판정이라 Optional은 늘 참이다 — 필드는 내부에 따라 달라지므로
// 필드 하나를 판정할 때는 FieldIsAggregate를 쓴다(Optional<단일 값>은 aggregate가 아니다).
constexpr bool ValueKindIsAggregate(ValueKind kind) noexcept {
    const uint32 properties = ValueKindProperties(kind);
    return (properties & detail::ValueKindBit(ValueKindProperty::Field)) != 0 &&
           (properties & detail::ValueKindBit(ValueKindProperty::WholeFieldValue)) == 0;
}

// 필드 값 형식이다. type은 Enum·Struct 스키마 ID, ObjectReference·WeakObjectReference·
// SoftObjectReference의 기대 대상 타입(0 = Object), ClassReference의 Base 클래스 ID(0이 아니어야
// 한다)에만 쓰고 그 밖은 0이어야 한다(Array의 value.type도 0 — 원소 대상은 element.type).
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
    // ObjectReference(또는 원소가 ObjectReference인 Array)다. 접근기 없는 Opaque 필드는 수동
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
    // maxElements는 0이다. 그 밖의 필드는 Opaque·0이어야 한다.
    ValueType element{};
    uint32 maxElements = 0;
};

// 필드 값의 대상 형식이다: 컨테이너 종류(ValueKindIsContainer — Array·Optional) 필드면 원소·내부
// 형식(element), 아니면 값 형식(value). enum·struct·참조 대상 해석(TypeRegistry의 의존·FieldEnum·
// FieldStruct, codec 검증, 그래프 기대 타입)이 같은 규칙을 쓴다. 반환 참조는 field의 수명을 따른다.
//
// 사용 예:
//   const ValueType& target = FieldTargetType(field);
//   if (target.kind == ValueKind::Enum) { ... }   // Enum 필드이거나 enum 원소 배열·Optional<enum>
constexpr const ValueType& FieldTargetType(const FieldSchema& field) noexcept {
    return ValueKindIsContainer(field.value.kind) ? field.element : field.value;
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

// 필드 경로의 한 단계다(7절 PropertyPath, 6.4절 진단 경로): 필드 ID와, 배열 원소면 그 index.
// 원소 index는 배열 필드의 단계에 둔다([slots[2], material] — slots 단계가 index 2를 가진다).
struct PathStep {
    Guid fieldId;
    uint32 index = 0;
    bool indexed = false;
};

// 속성 경로의 최대 단계 수다(Docs/Design/ReflectionFoundations.ko.md 7.1절): 최상위 필드 1 + struct
// 중첩 MaxStructDepth(16) — codec 위치 프레임 수와 같다. 원소 index는 단계를 늘리지 않는다.
inline constexpr uint32 MaxPropertyPathSteps = MaxStructDepth + 1;

// 객체 하나 안의 속성 경로다(7절 — FieldId 체인 + 원소 index). 참조를 따라 다른 객체로 들어가지
// 않는다(객체 지정은 handle이 맡는다). steps는 비소유 뷰이며 쓰는 동안 유효해야 한다. 문자열 경로
// ("slots[2].material")는 ParsePropertyPath(Object/PropertyPath.h)가 이 표현으로 바꾼다.
//
// 사용 예:
//   const PathStep steps[] = {{SlotsId, 2, true}, {MaterialId}};
//   PropertyPath path{steps};
struct PropertyPath {
    Span<const PathStep> steps;
};

// 단계가 정규형인지다: index가 없는 단계(indexed == false)는 index가 0이어야 한다. 경로 연산은
// 정규형이 아닌 단계를 InvalidArgument로 거부한다 — 같은 위치를 가리키는 경로가 한 표현만 가지므로
// 경로 동등성(SamePropertyPath)으로 Undo 기록을 병합할 수 있다.
constexpr bool IsCanonicalPathStep(const PathStep& step) noexcept {
    return step.indexed || step.index == 0;
}

// 두 경로가 같은 위치인지다(단계마다 필드 ID·index·indexed가 같다). 정규형 경로끼리의 비교다.
constexpr bool SamePropertyPath(PropertyPath left, PropertyPath right) noexcept {
    if (left.steps.Size() != right.steps.Size()) {
        return false;
    }

    for (usize index = 0; index < left.steps.Size(); ++index) {
        const PathStep& a = left.steps[index];
        const PathStep& b = right.steps[index];
        if (!(a.fieldId == b.fieldId) || a.index != b.index || a.indexed != b.indexed) {
            return false;
        }
    }

    return true;
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
};

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
