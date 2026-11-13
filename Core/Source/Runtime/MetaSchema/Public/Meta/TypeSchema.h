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
    // B3 이후의 값 종류다. Enum·Struct·Array는 각 단계(B3b·B4·B5)에서 등록을 연다.
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

} // namespace rocks
