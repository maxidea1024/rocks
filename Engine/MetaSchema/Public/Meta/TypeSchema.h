#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>

namespace rocks {

using TypeId = Guid;
using ContextId = Guid;

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
    ObjectReference
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
    ValueKind valueKind = ValueKind::Opaque;
    // ObjectReference의 대상 schema다. 0은 기반 Object를 뜻하며 다른 kind에서는 0이어야 한다.
    TypeId referencedType{};
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
};

} // namespace rocks
