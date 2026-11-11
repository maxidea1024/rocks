#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>

namespace rocks {

class TypeRegistry;
struct RuntimeTypeOps;

// 불변 스키마와 콜백 코드의 수명을 유지한다. lease가 있으면 Unregister는 Busy다.
// Registry와 그 Allocator가 모든 lease보다 오래 살아야 한다.
// 서로 다른 lease의 Clone/Reset은 동시 호출 가능하나 같은 래퍼의 동시 변경은 금지한다.
class TypeLease {
public:
    TypeLease() noexcept = default;
    TypeLease(TypeLease&& other) noexcept;
    TypeLease& operator=(TypeLease&& other) noexcept;
    TypeLease(const TypeLease&) = delete;
    TypeLease& operator=(const TypeLease&) = delete;
    ~TypeLease();

    Result<TypeLease> Clone() const noexcept;
    // 반환 참조는 이 lease 또는 같은 레코드를 지키는 다른 lease가 살아 있는 동안 유효하다.
    // 임시 lease에서 빌린 참조를 반환하는 호출은 금지한다. 빈 lease 접근은 즉시 종료한다.
    const TypeSchema& Schema() const& noexcept;
    const TypeSchema& Schema() const&& = delete;
    const RuntimeTypeOps& Ops() const& noexcept;
    const RuntimeTypeOps& Ops() const&& = delete;

    // ID는 값으로 반환하므로 lease 종료 후에도 보관할 수 있다. 빈 lease는 Fatal이다.
    TypeId Id() const noexcept;

    // 이름은 이 lease 또는 같은 레코드를 지키는 다른 lease에 종속된 UTF-8 byte view다.
    // 임시 lease에 호출하면 view가 곧바로 무효화될 수 있어 금지한다. 빈 lease는 Fatal이다.
    StringView Name() const& noexcept;
    StringView Name() const&& = delete;

    // immutable schema ancestry 조회다. 빈 lease는 Fatal이다. native pointer 변환이나
    // Borrow<base> 허용을 뜻하지 않는다.
    bool IsA(TypeId expected) const noexcept;

    bool IsValid() const noexcept {
        return record_ != nullptr;
    }

    void Reset() noexcept;

private:
    friend class TypeRegistry;

    TypeLease(TypeRegistry* owner, void* record) noexcept : owner_(owner), record_(record) {
    }

    TypeRegistry* owner_ = nullptr;
    void* record_ = nullptr;
};

} // namespace rocks
