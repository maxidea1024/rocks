#include "TypeRegistryInternal.h"
#include "TypeRegistryRegisterInternal.h"

#include <Base/PlacementNew.h>
#include <Object/TypeRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

// TypeRegistry enum 등록(RegisterEnum)과 enum 도구 이름 검사(IsEnumToolName·EnumToolNamesCollide)다
// (Docs/Design/ReflectionFoundations.ko.md 3.4절). 등록 파일이 함께 쓰는 도우미는
// TypeRegistryRegisterInternal.h다.
namespace rocks {

namespace {

using detail::AddBytes;
using detail::CopyBytes;
using detail::IsAsciiIdentifier;
using detail::RecordTag;

// enum 도구 이름·접두의 문자 집합이다: [A-Za-z0-9_]+ (숫자로 시작할 수 있다).
bool IsEnumToolName(Span<const char> text) noexcept {
    if (text.Size() == 0) {
        return false;
    }

    for (usize index = 0; index < text.Size(); ++index) {
        const char c = text[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_')) {
            return false;
        }
    }

    return true;
}

// 접두 유무를 합친 두 도구 이름 집합이 대소문자 무시로 겹치는지다(a, prefix+a)와(b, prefix+b).
bool EnumToolNamesCollide(Span<const char> left, Span<const char> right,
                          Span<const char> prefix) noexcept {
    if (EqualsIgnoringAsciiCase(left, right)) {
        return true;
    }

    // prefix+left == right 또는 left == prefix+right
    const auto prefixed = [&prefix](Span<const char> shorter, Span<const char> longer) noexcept {
        if (prefix.Size() == 0 || longer.Size() != prefix.Size() + shorter.Size()) {
            return false;
        }

        for (usize index = 0; index < prefix.Size(); ++index) {
            if (detail::AsciiLower(prefix[index]) != detail::AsciiLower(longer[index])) {
                return false;
            }
        }

        return detail::EnumNameEquals(shorter, longer, prefix.Size());
    };
    return prefixed(left, right) || prefixed(right, left);
}

constexpr usize MaxEnumerators = 4096;

} // namespace

Result<TypeRegistrationToken> TypeRegistry::RegisterEnum(const EnumSchema& schema,
                                                         RuntimeEnumOps ops) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::WrongThread);
    }

    int64 minimum = 0;
    int64 maximum = 0;
    const bool flags = HasFlag(schema.flags, EnumFlags::Flags);
    if (schema.id == Guid{} || schema.name.Size() == 0 ||
        !StringView(schema.name.Data(), schema.name.Size()).ValidateUtf8() ||
        !EnumUnderlyingRange(schema.underlying, minimum, maximum) ||
        (schema.flags & ~static_cast<uint32>(EnumFlags::Flags)) != 0 || schema.values.Size() == 0 ||
        schema.values.Size() > MaxEnumerators ||
        (schema.toolPrefix.Size() != 0 && !IsEnumToolName(schema.toolPrefix)) ||
        ops.nativeType == nullptr) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    usize bytes =
        (sizeof(Record) + alignof(EnumeratorSchema) - 1) & ~(alignof(EnumeratorSchema) - 1);
    const usize valuesOffset = bytes;
    bytes += schema.values.Size() * sizeof(EnumeratorSchema);
    if (!AddBytes(bytes, schema.name.Size()) || !AddBytes(bytes, schema.toolPrefix.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
    }

    for (usize index = 0; index < schema.values.Size(); ++index) {
        const EnumeratorSchema& value = schema.values[index];
        if (!IsAsciiIdentifier(value.name) ||
            (value.toolName.Size() != 0 && !IsEnumToolName(value.toolName)) ||
            !StringView(value.displayName.Data(), value.displayName.Size()).ValidateUtf8() ||
            (value.flags & ~static_cast<uint32>(EnumeratorFlags::Hidden)) != 0 ||
            value.value < minimum || value.value > maximum || (flags && value.value < 0)) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }

        // 값 별칭과 (접두 유무를 합친) 도구 이름 중복은 받지 않는다.
        for (usize previous = 0; previous < index; ++previous) {
            const EnumeratorSchema& other = schema.values[previous];
            if (other.value == value.value ||
                EnumToolNamesCollide(EffectiveToolName(value), EffectiveToolName(other),
                                     schema.toolPrefix)) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }
        }

        if (!AddBytes(bytes, value.name.Size()) || !AddBytes(bytes, value.toolName.Size()) ||
            !AddBytes(bytes, value.displayName.Size())) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
        }
    }

    {
        platform::LockGuard lock(gate_);
        if (closed_ || mutating_ || nextGeneration_ == 0) {
            return Result<TypeRegistrationToken>::Failure(closed_     ? ErrorCode::ShuttingDown
                                                          : mutating_ ? ErrorCode::Busy
                                                                      : ErrorCode::Overflow);
        }

        // 같은 C++ enum을 다른 ID로 다시 등록하지 않는다. 생성 thunk는 GeneratedType<E>::Id()
        // 하나만 받으므로 별칭 ID를 가리키는 필드는 쓸 수도 저장할 수도 없게 된다.
        if (FindLocked(schema.id) != nullptr ||
            FindNativeLocked(ops.nativeType, TypeKind::Enum) != nullptr) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
        }

        mutating_ = true;
    }

    // 색인 공간은 레코드보다 먼저 준비한다(실패하면 아무것도 바꾸지 않는다).
    IndexGrowth growth(allocator_);
    if (auto prepared = PrepareIndexes(growth, true); !prepared) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(prepared.Error());
    }

    auto memory = allocator_.Allocate(bytes, alignof(Record), RecordTag);
    if (!memory) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(memory.Error());
    }

    if (!memory.Value()) {
        platform::Fatal("Allocator returned null TypeRegistry record");
    }

    Record* record = ::new (memory.Value()) Record();
    auto* values =
        reinterpret_cast<EnumeratorSchema*>(static_cast<uint8*>(memory.Value()) + valuesOffset);
    uint8* cursor = reinterpret_cast<uint8*>(values + schema.values.Size());
    record->kind = TypeKind::Enum;
    record->schema.id = schema.id;
    record->schema.name = CopyBytes(schema.name, cursor);
    record->enumSchema.id = schema.id;
    record->enumSchema.name = record->schema.name;
    record->enumSchema.underlying = schema.underlying;
    record->enumSchema.flags = schema.flags;
    record->enumSchema.toolPrefix = CopyBytes(schema.toolPrefix, cursor);
    for (usize index = 0; index < schema.values.Size(); ++index) {
        ::new (values + index) EnumeratorSchema(schema.values[index]);
        values[index].name = CopyBytes(schema.values[index].name, cursor);
        values[index].toolName = CopyBytes(schema.values[index].toolName, cursor);
        values[index].displayName = CopyBytes(schema.values[index].displayName, cursor);
    }

    record->enumSchema.values = {values, schema.values.Size()};
    record->ops.nativeType = ops.nativeType;
    record->ops.module = Move(ops.module);

    platform::LockGuard lock(gate_);
    PublishLocked(record, growth);
    mutating_ = false;
    return Result<TypeRegistrationToken>::Success({domain_, record->generation, schema.id});
}

} // namespace rocks
