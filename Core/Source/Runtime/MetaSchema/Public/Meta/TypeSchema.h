#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>

namespace rocks {

using TypeId = Guid;
using ContextId = Guid;

// flags는 저장 형식과 별개인 메타데이터 정책이다. 알 수 없는 비트는 등록 단계에서 거부한다.
//
// TypeFlags:
//   Abstract  — 등록할 수 있으나 생성할 수 없다(construct가 null).
//   WorldOnly — World 쪽에서만 쓰는 타입이다. 상속된다(base가 WorldOnly면 파생도 이 플래그가
//               있어야 등록된다). 에셋 객체 Registry의 생성 거부는
//               RegistryPolicy::rejectWorldOnly(Object/ObjectRegistry.h)다.
enum class TypeFlags : uint32 {
    None = 0,
    Abstract = 1,
    WorldOnly = 2
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
    Digest256            // rocks::Sha256Digest(Core/Encoding/Sha256.h), wire Bytes(정확히 32 byte)
};

// 필드 값 형식이다. type은 Enum·Struct 스키마 ID, ObjectReference·SoftObjectReference의 기대 대상
// 타입(0 = Object)에만 쓰고 그 밖은 0이어야 한다(Array의 value.type도 0 — 원소 대상은
// element.type).
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
    // MaxArrayElements 이하, 저장·경로 쓰기에서 건다). 원소는 스칼라·Guid·Utf8String·
    // ObjectReference·Enum·Struct·AssetId·SoftObjectReference·Digest256이고 element.type은 Enum·
    // Struct면 필수, 두 참조는 기대 타입(선택), 그 밖은 0이다. 배열이 아닌 필드는 Opaque·0이어야
    // 한다.
    ValueType element{};
    uint32 maxElements = 0;
};

// 필드 값의 대상 형식이다: Array 필드면 원소 형식(element), 아니면 값 형식(value). enum·struct·참조
// 대상 해석(TypeRegistry의 의존·FieldEnum·FieldStruct, codec 검증, 그래프 기대 타입)이 같은 규칙을
// 쓴다. 반환 참조는 field의 수명을 따른다.
//
// 사용 예:
//   const ValueType& target = FieldTargetType(field);
//   if (target.kind == ValueKind::Enum) { ... }   // Enum 필드이거나 enum 원소 배열
constexpr const ValueType& FieldTargetType(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::Array ? field.element : field.value;
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
};

// EnumFlags:
//   Flags — 비트 플래그 enum이다(ROCKS_ENUM(Flags)). 값은 선언 값들의 OR(0 포함)이면 유효하다.
//           열거자 값은 음수일 수 없다.
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
// I64다. underlying은 UInt8·UInt16·UInt32·Int32 중 하나이며 열거자 값은 그 범위 안에서 서로
// 다르다(별칭 금지). 등록은 TypeRegistry::RegisterEnum이다.
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

// underlying 폭의 값 범위를 minimum·maximum에 쓴다. 지원하지 않는 underlying이면 false다.
constexpr bool EnumUnderlyingRange(ValueKind underlying, int64& minimum, int64& maximum) noexcept {
    switch (underlying) {
    case ValueKind::UInt8:
        minimum = 0;
        maximum = 0xff;
        return true;
    case ValueKind::UInt16:
        minimum = 0;
        maximum = 0xffff;
        return true;
    case ValueKind::UInt32:
        minimum = 0;
        maximum = 0xffffffffll;
        return true;
    case ValueKind::Int32:
        minimum = -0x80000000ll;
        maximum = 0x7fffffffll;
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
