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
//               있어야 등록된다). 에셋 객체 Registry의 생성 거부 정책은 패키지 단계(A0-O)다.
enum class TypeFlags : uint32 {
    None = 0,
    Abstract = 1,
    WorldOnly = 2
};

// FieldFlags:
//   Transient  — GC 추적은 하되 저장 그래프에서 뺀다.
//   ReadOnly   — 일반 write를 거부하고 생성·복원의 staged write만 허용한다.
//   EditorOnly — 항상 컴파일되고 플래그만 단다. 쿠킹 데이터에서 빼는 것은 graph v2(A0-O)다.
//   Required   — typed 참조·소프트 참조 필드에만 쓴다. null 거부는 패키지 저장·쿠킹(A1).
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
    // B3 이후의 값 종류다. Enum은 B3b에서 열렸고 Struct·Array는 B4·B5에서 등록을 연다.
    Enum,
    Struct,
    Array,
    AssetId,             // rocks::AssetId(Object/AssetId.h), wire Guid
    SoftObjectReference, // rocks::SoftObjectRef<T>(Object/SoftObjectRef.h), wire
                         // SoftObjectReference
    Digest256            // rocks::Sha256Digest(Core/Encoding/Sha256.h), wire Bytes(정확히 32 byte)
};

// 필드 값 형식이다. type은 Enum·Struct 스키마 ID, ObjectReference·SoftObjectReference의 기대 대상
// 타입(0 = Object)에만 쓰고 그 밖은 0이어야 한다.
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
    // Array 필드의 원소 형식과 길이 상한이다(B5). 그 밖의 필드는 Opaque·0이어야 한다.
    ValueType element{};
    uint32 maxElements = 0;
};

// 필드 경로의 한 단계다(7절 PropertyPath, 6.4절 진단 경로): 필드 ID와, 배열 원소면 그 index.
struct PathStep {
    Guid fieldId;
    uint32 index = 0;
    bool indexed = false;
};

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

constexpr char EnumAsciiLower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// left가 right[offset..]과 ASCII 대소문자 무시로 같은지다.
constexpr bool EnumNameEquals(Span<const char> left, Span<const char> right,
                              usize offset = 0) noexcept {
    if (offset > right.Size() || left.Size() != right.Size() - offset) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (EnumAsciiLower(left[index]) != EnumAsciiLower(right[offset + index])) {
            return false;
        }
    }

    return true;
}

} // namespace detail

// 열거자의 실효 도구 이름이다(toolName이 비면 C++ 이름).
constexpr Span<const char> EffectiveToolName(const EnumeratorSchema& value) noexcept {
    return value.toolName.Size() != 0 ? value.toolName : value.name;
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
        prefixed = detail::EnumAsciiLower(toolName[index]) ==
                   detail::EnumAsciiLower(schema.toolPrefix[index]);
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
