#include "TypeRegistryInternal.h"

#include <Object/ClassRef.h>
#include <Object/TypeRegistry.h>

// TypeRegistry의 타입 도구 이름 색인 조회(AcquireByToolName)와 파생 클래스 조회
// (AcquireDerivedClass)다(리플렉션 B9d2 — Docs/Design/ReflectionExtensions.ko.md 5.2절). 색인의
// 준비·게시·삭제는 TypeRegistry.cpp(PrepareIndexes·PublishLocked·Unregister·Shutdown), 유일성
// 검사는 등록(TypeRegistryRegister.cpp)이다.
namespace rocks {

uint64 TypeRegistry::ToolNameHash::operator()(StringView name) const noexcept {
    // ASCII 대소문자를 접은 FNV-1a(Core 공용 — ToolNameEqual과 같은 같음)를 섞는다.
    return MixHash(HashStringBytesIgnoringAsciiCase(name));
}

bool TypeRegistry::ToolNameEqual::operator()(StringView left, StringView right) const noexcept {
    return EqualsIgnoringAsciiCase(Span<const char>(left.Data(), left.ByteCount()),
                                   Span<const char>(right.Data(), right.ByteCount()));
}

TypeRegistry::Record* TypeRegistry::FindToolNameLocked(Span<const char> toolName) const noexcept {
    if (toolName.Size() == 0) {
        return nullptr;
    }

    Record* const* found = byToolName_.Find(StringView(toolName.Data(), toolName.Size()));
    return found != nullptr ? *found : nullptr;
}

Result<TypeLease> TypeRegistry::AcquireByToolName(Span<const char> toolName) noexcept {
    if (toolName.Size() == 0) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<TypeLease>::Failure(ErrorCode::ShuttingDown);
    }

    Record* record = FindToolNameLocked(toolName);
    if (record == nullptr) {
        return Result<TypeLease>::Failure(ErrorCode::NotFound);
    }

    return LeaseLocked(record);
}

Result<TypeLease> TypeRegistry::AcquireDerivedClass(TypeId type, TypeId base) noexcept {
    if (base == TypeId{}) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    return detail::CheckClassReferenceLease(Acquire(type), base, nullptr);
}

} // namespace rocks
