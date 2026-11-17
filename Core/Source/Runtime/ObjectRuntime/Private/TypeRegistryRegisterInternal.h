#pragma once

#include <Object/TypeRegistry.h>

// TypeRegistry 등록 구현 파일(TypeRegistryRegister.cpp·TypeRegistryRegisterEnum.cpp·
// TypeRegistryRegisterStruct.cpp)이 함께 쓰는 내부 도우미다. 레코드 블록 크기 누계(AddBytes)·
// 문자열 복사(CopyBytes)·할당 태그(RecordTag)와, 클래스·enum·struct 서술 검사가 함께 쓰는
// 식별자·필드 목록 판정(IsAsciiIdentifier·ValidateFieldList·CarriesValueNativeType — 정의는
// TypeRegistryRegister.cpp)을 둔다.
namespace rocks::detail {

inline bool AddBytes(usize& total, usize count) noexcept {
    if (count > MaxSize - total) {
        return false;
    }

    total += count;
    return true;
}

template <class T>
Span<const T> CopyBytes(Span<const T> source, uint8*& cursor) noexcept {
    T* destination = reinterpret_cast<T*>(cursor);
    for (usize index = 0; index < source.Size(); ++index) {
        destination[index] = source[index];
    }

    cursor += source.Size() * sizeof(T);
    return {destination, source.Size()};
}

// 비어 있지 않은 ASCII 식별자([A-Za-z_][A-Za-z0-9_]*)인지다(필드 도구 이름·열거자 이름).
bool IsAsciiIdentifier(Span<const char> text) noexcept;

// 클래스·struct가 함께 쓰는 필드 서술 검사다(Docs/Design/ReflectionFoundations.ko.md 4.6절). 필드
// 하나의 규칙(플래그·값 종류·Required·도구 이름)과 필드 사이 규칙(같은 ID·실효 도구 이름은
// AlreadyExists)을 본다. structMembers면 값 종류를 struct 멤버 자리로 판정한다. typedFields에
// Opaque가 아닌 필드 수를 쓴다.
Result<void> ValidateFieldList(Span<const FieldSchema> fields, bool structMembers,
                               usize& typedFields) noexcept;

// 대상 형식(배열이면 원소)이 Enum·Struct인 필드만 값의 native 키를 싣는다.
bool CarriesValueNativeType(const FieldSchema& field) noexcept;

// 키가 Enum인 Map 필드만 키의 native 키를 싣는다(리플렉션 B9f2).
bool CarriesKeyNativeType(const FieldSchema& field) noexcept;

// 파생 스키마의 필드 목록 derived가 base의 모든 필드를 같은 안정 ID·표현(종류·대상·원소·키·
// maxElements·플래그·실효 도구 이름)으로 담는지다(펼침 상속 — 클래스 Register와 struct
// RegisterStruct(리플렉션 B9k)가 같이 쓴다).
bool ContainsInheritedFields(Span<const FieldSchema> base,
                             Span<const FieldSchema> derived) noexcept;
// 레코드 블록 할당 태그다('TYPE').
inline constexpr uint32 RecordTag = 0x54595045u;

} // namespace rocks::detail
