#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>

namespace rocks {

class TypeRegistry;
struct RuntimeTypeOps;

// TypeRegistry 레코드의 종류다. Class는 Object 파생 타입(Register·Acquire), Enum은 리플렉션
// enum(RegisterEnum·AcquireValueType), Struct는 리플렉션 struct(B4)다.
enum class TypeKind : uint8 {
    Class,
    Struct,
    Enum
};

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

    // 레코드 종류다. Acquire는 Class만, AcquireValueType은 Enum·Struct만 돌려준다. 빈 lease는
    // Fatal이다. Enum lease의 Schema()는 id·name만 있고 필드가 없으며 Ops()는 nativeType·module만
    // 있다.
    TypeKind Kind() const noexcept;

    // Enum lease의 enum 스키마다. Enum이 아니거나 빈 lease면 Fatal이다. 수명은 Schema()와 같다.
    const EnumSchema& Enum() const& noexcept;
    const EnumSchema& Enum() const&& = delete;

    // Class lease에서 fieldIndex(Schema().fields의 위치) 필드가 가리키는 enum의 스키마다. 그
    // 필드가 Enum 값이 아니면 nullptr이다. 등록이 그 enum을 의존으로 붙잡으므로(Unregister는 Busy)
    // 반환 참조는 이 lease 수명 동안 유효하다. 범위 밖 index나 빈 lease는 Fatal이다.
    //
    // 사용 예(저장된 값 검증):
    //   const EnumSchema* values = lease.FieldEnum(index);
    //   if (values && !IsDeclaredEnumValue(*values, stored)) { ... }
    const EnumSchema* FieldEnum(usize fieldIndex) const& noexcept;
    const EnumSchema* FieldEnum(usize fieldIndex) const&& = delete;

    // 이름은 이 lease 또는 같은 레코드를 지키는 다른 lease에 종속된 UTF-8 byte view다.
    // 임시 lease에 호출하면 view가 곧바로 무효화될 수 있어 금지한다. 빈 lease는 Fatal이다.
    StringView Name() const& noexcept;
    StringView Name() const&& = delete;

    // immutable schema ancestry 조회다. 빈 lease는 Fatal이다. 스키마 ID 기준이므로 같은 native
    // 타입을 다른 TypeId로 등록한 별칭은 구분된다.
    bool IsA(TypeId expected) const noexcept;

    // 이 타입이 nativeType(NativeTypeKey<T>())의 C++ 타입이거나 그 파생인지다. nullptr(Object)는
    // 항상 참이다. Resolve<T>·SetReference·NewObject<T>가 이 규칙으로 기반 타입 접근을 허용한다.
    // 단일 비가상 상속만 등록되므로 참이면 Object*에서 T*로의 static_cast가 안전하다. 빈 lease는
    // Fatal.
    bool IsNativeA(const void* nativeType) const noexcept;

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
