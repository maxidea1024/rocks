#pragma once

#include <Base/TypeTraits.h>
#include <Core/Move.h>
#include <Meta/GeneratedType.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>

// 실행 중 정해지는 리플렉션 struct 값 하나를 담는 값 타입 InstancedStruct<>다(리플렉션 B9j —
// Docs/Design/ReflectionExtensions.ko.md 11절, UE FInstancedStruct 대응). Sparks 데이터 테이블 행
// 저장(`OrderedMap<Name, InstancedStruct<>>`)·데이터 에셋처럼 필드 형식이 정해지지 않은 struct 값을
// 둘 때 쓴다.
namespace rocks {

namespace detail {

// struct가 InstancedStruct에 담길 수 있는지의 유일한 판정이다(Emplace·쓰기·형 바꾸기·decode 1단계가
// 같이 쓴다 — Docs/Design/ReflectionExtensions.ko.md 11절 담기 조건). 강한 참조(값 블록이 소유 객체
// 밖이라 참조 쓰기의 범위 검사를 지날 수 없다)·Registry 컨테이너(ObjectRefArray·StructRefArray·
// ObjectRefMap)·InstancedStruct(중첩 금지 — 깊이를 등록 값으로 정한다)·Required 멤버(2c의 Required
// 실효 값 검사가 등록 형질로 최상위 필드를 고른다)를 담는 struct와 WorldOnly 소유자가 필요한
// struct(NATIVE 멤버 형질 — 담긴 타입은 실행 중 정해지므로 소유 객체 클래스로 판정할 수 없다)는
// InvalidArgument다. 빈 view도 InvalidArgument다.
//
// 사용 예:
//   if (auto allowed = detail::CheckInstancedStructContent(lease.Struct()); !allowed) { ... }
inline Result<void> CheckInstancedStructContent(StructView view) noexcept {
    if (view.IsEmpty() || view.ContainsStrongReference() || view.ContainsReferenceArray() ||
        view.ContainsInstancedStruct() || view.ContainsRequired() ||
        view.RequiresWorldOnlyOwner()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// InstancedStruct가 놓인 위치 깊이 position(그 필드를 감싼 struct 단계 수 — 최상위 필드면 0)과 담긴
// struct의 깊이(StructView::Depth)의 합이 MaxStructDepth 이하인지다(같은 문서 11절 깊이 규칙). 합이
// 넘으면 InvalidArgument다. codec 위치 프레임(1 + 16)·Record 스택·MaxPropertyPathSteps가 이 합으로
// 지켜진다.
inline Result<void> CheckInstancedStructDepth(uint32 position, StructView view) noexcept {
    if (view.IsEmpty() || position > MaxStructDepth || view.Depth() > MaxStructDepth - position) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

struct InstancedStructAccess;

} // namespace detail

// 리플렉션 struct 값 하나 또는 비어 있음이다. PROPERTY로 두면 값 종류 InstancedStruct이고(클래스
// 필드·struct 멤버·Array·정적 배열 원소·Map 값 — 1.3절 성질 표) 스키마 value.type은 Base struct
// ID다(Base = void는 0 — 제한 없음). InstancedStruct<Base>(리플렉션 B9k)는 Base 자신이나 그 파생
// struct만 담는다: Emplace<S>는 static_assert(IsBaseOfV), lease 판 Emplace·쓰기·decode는 스키마
// 사슬(TypeLease::IsA)로 본다(위반 InvalidArgument). Base는 담을 수 있는 struct여야 한다.
//
// 구성: 담긴 struct의 TypeLease(값이 사는 동안 그 타입 등록을 붙잡는다 — Unregister는 Busy), 값
// 블록(allocator로 할당한 그 struct 값), 그 allocator. 이동 전용이고 깊은 복사는 TryClone이다.
// 담을 수 있는 struct는 detail::CheckInstancedStructContent 조건을 지킨다(강한 참조·Registry
// 컨테이너·InstancedStruct·Required·WorldOnly 소유자 요구 없음). 위치 깊이 + 담긴 struct 깊이 ≤
// MaxStructDepth 규칙은 놓인 자리를 아는 쓰기·경로 형 바꾸기·decode·encode가 본다(C++가 Emplace로
// 깊은 자리에 직접 넣은 위반은 저장 때 InvalidArgument로 드러난다).
//
// 저장: wire InstancedStruct(17) — 비어 있으면 빈 payload, 아니면 담긴 struct의 Record(schema =
// struct ID, version = 그 struct 버전, 버전 마이그레이션은 중첩 struct와 같다). GC 간선이 없다(담긴
// struct가 강한 참조를 담지 않는다). TypeLease는 이 값이 쥔 TypeRegistry 레코드를 가리키므로
// TypeRegistry·allocator가 값보다 오래 살아야 한다(객체 필드면 Registry 수명).
//
// 사용 예:
//   InstancedStruct<> row;
//   auto made = row.Emplace<WeaponRow>(registry.Types(), registry.ObjectAllocator());
//   if (WeaponRow* weapon = row.Get<WeaponRow>()) { weapon->damage = 10; }
//   row.Reset();   // 비어 있음
template <class Base = void>
class InstancedStruct {
public:
    InstancedStruct() noexcept = default;

    InstancedStruct(InstancedStruct&& other) noexcept
        : type_(Move(other.type_)), value_(other.value_), allocator_(other.allocator_) {
        other.value_ = nullptr;
        other.allocator_ = nullptr;
    }

    InstancedStruct& operator=(InstancedStruct&& other) noexcept {
        if (this != &other) {
            Reset();
            type_ = Move(other.type_);
            value_ = other.value_;
            allocator_ = other.allocator_;
            other.value_ = nullptr;
            other.allocator_ = nullptr;
        }

        return *this;
    }

    InstancedStruct(const InstancedStruct&) = delete;
    InstancedStruct& operator=(const InstancedStruct&) = delete;

    ~InstancedStruct() {
        Reset();
    }

    // 비어 있는지다(담긴 값이 없다).
    bool IsEmpty() const noexcept {
        return value_ == nullptr;
    }

    // 담긴 struct의 lease다(비어 있으면 빈 lease — IsValid가 거짓). 반환 참조는 이 값이 바뀌거나
    // 소멸할 때까지 유효하다.
    const TypeLease& Type() const noexcept {
        return type_;
    }

    // 담긴 struct의 view다(비어 있으면 빈 view). 이 값이 바뀌거나 소멸할 때까지 유효하다.
    StructView View() const noexcept {
        return type_.IsValid() ? type_.Struct() : StructView{};
    }

    // 값 블록의 allocator다(비어 있으면 nullptr).
    Allocator* GetAllocator() const noexcept {
        return allocator_;
    }

    // type(struct lease — AcquireValueType(id, TypeKind::Struct)·AcquireStructByNative)의 기본
    // 값(construct — noexcept S(Allocator&)면 allocator를 넘긴다)으로 바꾼다. 새 값을 allocator로
    // 다 만든 뒤 옛 값을 소멸하고 반환하므로 실패하면 이 값은 그대로다(type은 소비된다). 실패:
    // 빈 lease·struct가 아닌 lease·담을 수 없는 struct(detail::CheckInstancedStructContent)
    // InvalidArgument, 할당 실패는 그 오류(OutOfMemory 등).
    //
    // 사용 예:
    //   auto lease = types.AcquireValueType(rowType, TypeKind::Struct);
    //   if (lease) { auto made = row.Emplace(Move(lease.Value()), allocator); }
    Result<void> Emplace(TypeLease type, Allocator& allocator) noexcept {
        if (!type.IsValid() || type.Kind() != TypeKind::Struct) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto allowed = detail::CheckInstancedStructContent(type.Struct()); !allowed) {
            return allowed;
        }

        // InstancedStruct<Base>(리플렉션 B9k)는 Base 자신이나 그 파생 struct만 담는다(스키마 사슬
        // IsA).
        if constexpr (!IsSameV<Base, void>) {
            if (!type.IsA(GeneratedType<Base>::Id())) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        const RuntimeStructOps& ops = type.StructOps();
        auto memory = allocator.Allocate(ops.size, ops.alignment, BlockTag);
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        ops.construct(memory.Value(), allocator);
        Reset();
        type_ = Move(type);
        value_ = memory.Value();
        allocator_ = &allocator;
        return Result<void>::Success();
    }

    // 리플렉션 struct S의 기본 값으로 바꾼다. S의 등록은 types에서 native 키로 찾는다
    // (AcquireStructByNative — 미등록 NotFound). S가 담을 수 없는 struct면(강한 참조·Registry
    // 컨테이너·InstancedStruct·Required — 생성 형질) 컴파일 오류다. 그 밖의 계약은 lease판
    // Emplace와 같다(실패하면 이 값은 그대로).
    //
    // 사용 예:
    //   auto made = row.Emplace<WeaponRow>(registry.Types(), registry.ObjectAllocator());
    template <class S>
    Result<void> Emplace(TypeRegistry& types, Allocator& allocator) noexcept {
        static_assert(!GeneratedType<S>::ContainsStrongReference &&
                          !GeneratedType<S>::ContainsReferenceArray,
                      "InstancedStruct cannot hold a struct with strong object references");
        static_assert(!GeneratedType<S>::ContainsInstancedStruct,
                      "InstancedStruct cannot hold a struct that holds an InstancedStruct");
        static_assert(!GeneratedType<S>::ContainsRequired,
                      "InstancedStruct cannot hold a struct with Required members");
        static_assert(IsSameV<Base, void> || IsBaseOfV<Base, S>,
                      "InstancedStruct<Base> holds only Base or a struct derived from Base");
        auto lease = types.AcquireStructByNative(NativeTypeKey<S>());
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        return Emplace(Move(lease.Value()), allocator);
    }

    // 담긴 값이 리플렉션 struct S(native 키 대조)면 그 주소, 아니면(비어 있음 포함) nullptr이다.
    // 반환 포인터는 이 값이 바뀌거나 소멸할 때까지 유효하다.
    template <class S>
    S* Get() noexcept {
        return value_ && type_.StructOps().nativeType == NativeTypeKey<S>()
                   ? static_cast<S*>(value_)
                   : nullptr;
    }

    template <class S>
    const S* Get() const noexcept {
        return value_ && type_.StructOps().nativeType == NativeTypeKey<S>()
                   ? static_cast<const S*>(value_)
                   : nullptr;
    }

    // 비운다(담긴 값을 소멸·반환하고 lease를 놓는다).
    void Reset() noexcept {
        if (value_) {
            type_.StructOps().destroy(value_);
            if (!allocator_->Deallocate(value_)) {
                platform::Fatal("InstancedStruct value block ownership mismatch");
            }

            value_ = nullptr;
            allocator_ = nullptr;
        }

        type_.Reset();
    }

    // 깊은 복사를 allocator로 만든다(비어 있으면 빈 값). 새 lease(Clone)·새 블록·기본 값 위의
    // 깊은 복사(RuntimeStructOps::copy)다. 실패: lease 복제의 오류(ShuttingDown·Overflow), 복사할
    // 수 없는 struct(copy가 null) Unsupported, 할당 실패는 그 오류. 실패해도 이 값은 그대로다.
    //
    // 사용 예:
    //   auto copy = row.TryClone(allocator);
    //   if (!copy) { return Result<void>::Failure(copy.Error()); }
    Result<InstancedStruct> TryClone(Allocator& allocator) const noexcept {
        InstancedStruct clone;
        if (!value_) {
            return Result<InstancedStruct>::Success(Move(clone));
        }

        const RuntimeStructOps& ops = type_.StructOps();
        if (!ops.copy) {
            return Result<InstancedStruct>::Failure(ErrorCode::Unsupported);
        }

        auto lease = type_.Clone();
        if (!lease) {
            return Result<InstancedStruct>::Failure(lease.Error());
        }

        auto memory = allocator.Allocate(ops.size, ops.alignment, BlockTag);
        if (!memory) {
            return Result<InstancedStruct>::Failure(memory.Error());
        }

        ops.construct(memory.Value(), allocator);
        clone.type_ = Move(lease.Value());
        clone.value_ = memory.Value();
        clone.allocator_ = &allocator;
        if (auto copied = ops.copy(clone.value_, value_, allocator); !copied) {
            return Result<InstancedStruct>::Failure(copied.Error());
        }

        return Result<InstancedStruct>::Success(Move(clone));
    }

private:
    friend struct detail::InstancedStructAccess;

    // 값 블록 할당 tag다('ISTR').
    static constexpr uint32 BlockTag = 0x49535452u;

    TypeLease type_;
    void* value_ = nullptr;
    Allocator* allocator_ = nullptr;
};

namespace detail {

template <class T>
struct InstancedStructField {
    static constexpr bool Supported = false;
};

// 리플렉션 InstancedStruct 필드(InstancedStruct<Base> — 리플렉션 B9j·B9k)다. 부분 특수화로만
// 고른다. BaseId는 스키마 value.type(Base = void면 0)이다.
template <class Base>
struct InstancedStructField<InstancedStruct<Base>> {
    static constexpr bool Supported = true;
    using BaseType = Base;

    static constexpr TypeId BaseId() noexcept {
        if constexpr (IsSameV<Base, void>) {
            return TypeId{};
        } else {
            return GeneratedType<Base>::Id();
        }
    }
};

// codec·경로 thunk가 담긴 값의 주소에 닿는 통로다(공개 API가 아니다 — 사용자는 Get<S>를 쓴다).
struct InstancedStructAccess {
    template <class Base>
    static void* Value(InstancedStruct<Base>& value) noexcept {
        return value.value_;
    }

    template <class Base>
    static const void* Value(const InstancedStruct<Base>& value) noexcept {
        return value.value_;
    }
};

} // namespace detail

} // namespace rocks
