#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>

namespace rocks {

class TypeRegistry;
class TypeLease;
struct RuntimeTypeOps;
struct RuntimeStructOps;
struct RuntimeStructFieldOps;

// 등록된 리플렉션 struct 하나의 비소유 view다(Docs/Design/ReflectionFoundations.ko.md 4.6절).
// 그것을 얻어 온 TypeLease(클래스·struct)가 사는 동안 유효하다 — 등록이 필드의 struct를 의존으로
// 붙잡으므로(Unregister는 Busy) 중첩 view도 바깥 lease 수명 동안 유효하다. 보관하지 않는다. 빈
// view에서 IsEmpty 외의 접근은 Fatal이다.
//
// 사용 예(codec이 struct 멤버를 스키마 순서로 돈다):
//   StructView view = lease.FieldStruct(fieldIndex);
//   for (usize index = 0; index < view.Schema().fields.Size(); ++index) {
//       const RuntimeStructFieldOps& member = view.FieldOps(index);
//   }
class StructView {
public:
    static constexpr usize NoField = ~usize(0);

    constexpr StructView() noexcept = default;

    bool IsEmpty() const noexcept {
        return record_ == nullptr;
    }

    // struct 스키마다(base 없음, id·version·name·fields).
    const TypeSchema& Schema() const noexcept;
    const RuntimeStructOps& StructOps() const noexcept;

    // 스키마 순서 schemaIndex 멤버의 함수표다. 범위 밖은 Fatal이다.
    const RuntimeStructFieldOps& FieldOps(usize schemaIndex) const noexcept;

    // 필드 ID의 스키마 위치다(정렬 색인 이진 탐색, 없으면 NoField).
    usize FieldIndex(Guid fieldId) const noexcept;

    // schemaIndex 멤버의 등록 enum·struct다(대상 형식 — 배열 멤버면 원소의 enum·struct, 그 종류가
    // 아니면 nullptr·빈 view).
    const EnumSchema* FieldEnum(usize schemaIndex) const noexcept;
    StructView FieldStruct(usize schemaIndex) const noexcept;

    // 등록이 계산한 값: 중첩 깊이(1 이상, MaxStructDepth 이하 — struct 원소 배열 멤버는 원소 깊이 +
    // 1), 강한 참조 멤버가 있는지(Transient 포함·중첩 포함·ObjectRefArray·StructRefArray 포함),
    // Required 멤버가 있는지(중첩 포함, 배열 멤버 제외 — 누락 struct 안 배열은 생성자 값이다), 소유
    // 객체가 WorldOnly여야 하는지(원소 struct 포함), Registry 컨테이너(ObjectRefArray·
    // StructRefArray) 멤버를 (중첩 struct까지) 담는지. 마지막 값이 참인 참조 struct는
    // StructRefArray의 원소가 될 수 없다.
    uint32 Depth() const noexcept;
    bool ContainsStrongReference() const noexcept;
    bool ContainsRequired() const noexcept;
    bool RequiresWorldOnlyOwner() const noexcept;
    bool ContainsReferenceArray() const noexcept;

private:
    friend class TypeRegistry;
    friend class TypeLease;

    explicit StructView(const void* record) noexcept : record_(record) {
    }

    const void* record_ = nullptr;
};

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
    // 있다. Struct lease의 Schema()는 id·version·name·fields이고 Ops()는 size·alignment·
    // nativeType·module이다(멤버 함수표 등은 StructOps()).
    TypeKind Kind() const noexcept;

    // Enum lease의 enum 스키마다. Enum이 아니거나 빈 lease면 Fatal이다. 수명은 Schema()와 같다.
    const EnumSchema& Enum() const& noexcept;
    const EnumSchema& Enum() const&& = delete;

    // Class·Struct lease에서 fieldIndex(Schema().fields의 위치) 필드가 가리키는 enum의 스키마다
    // (대상 형식 — enum 원소 배열이면 원소 enum). 그 필드가 Enum 값·enum 원소 배열이 아니면
    // nullptr이다. 등록이 그 enum을 의존으로 붙잡으므로(Unregister는 Busy)
    // 반환 참조는 이 lease 수명 동안 유효하다. 범위 밖 index나 빈 lease는 Fatal이다.
    //
    // 사용 예(저장된 값 검증):
    //   const EnumSchema* values = lease.FieldEnum(index);
    //   if (values && !IsDeclaredEnumValue(*values, stored)) { ... }
    const EnumSchema* FieldEnum(usize fieldIndex) const& noexcept;
    const EnumSchema* FieldEnum(usize fieldIndex) const&& = delete;

    // Class·Struct lease에서 fieldIndex 필드가 가리키는 등록 struct의 view다(대상 형식 — struct
    // 원소 배열이면 원소 struct). 그 필드가 Struct 값·struct 원소 배열이 아니면 빈 view다. 반환
    // view는 이 lease 수명 동안 유효하다. 범위 밖 index나 빈 lease는 Fatal이다.
    StructView FieldStruct(usize fieldIndex) const& noexcept;
    StructView FieldStruct(usize fieldIndex) const&& = delete;

    // Struct lease(AcquireValueType(id, TypeKind::Struct))의 view·실행 정보다. Struct가 아니거나 빈
    // lease면 Fatal이다.
    StructView Struct() const& noexcept;
    StructView Struct() const&& = delete;
    const RuntimeStructOps& StructOps() const& noexcept;
    const RuntimeStructOps& StructOps() const&& = delete;

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
