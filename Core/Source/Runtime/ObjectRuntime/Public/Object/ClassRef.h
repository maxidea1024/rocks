#pragma once

#include <Base/TypeTraits.h>
#include <Core/Hash.h>
#include <Meta/GeneratedType.h>
#include <Object/Object.h>
#include <Object/TypeRegistry.h>

// 클래스 참조 값 타입 ClassRef<Base>다(리플렉션 B9d2 —
// Docs/Design/ReflectionExtensions.ko.md 5.2절, UE TSubclassOf 대응). 영속 값은 클래스 TypeId
// 하나이고(UE는 UClass 객체 참조를 저장한다 — Rocks는 영속 ID와 실행 핸들을 나눈다,
// Docs/Design/RuntimeClassMetadata.ko.md 30행), Sparks의 `Class<T>`가 이것이다. 등록 핸들
// 캐시(세대·revision)는 RuntimeClassMetadata 2단계가 이 위에 더한다.
namespace rocks {

namespace detail {

// ClassRef 값 판정의 유일한 규칙이다(쓰기·encode·decode·Resolve가 같이 쓴다). acquired는 값의
// TypeId로 클래스 레코드를 연 결과(TypeRegistry::Acquire·ObjectRegistry::AcquireRegisteredType —
// 클래스가 아닌 레코드는 TypeMismatch)이고, base는 스키마의 Base 클래스 ID(0이면 보지 않는다),
// baseNative는 정적 Base의 NativeTypeKey<Base>()다(nullptr이면 보지 않는다 — Object). 값 클래스가
// base 자신이거나 그 파생(스키마 사슬 IsA)이고 baseNative의 C++ 파생(IsNativeA)이면 그 lease를
// 돌려준다. 실패: 미등록 NotFound(acquired의 오류 그대로), 클래스가 아닌 레코드(struct·enum)와
// 파생 위반 InvalidArgument, 그 밖의 acquired 오류(ShuttingDown·문맥 오류·lease 포화)는 그대로다.
// 추상 클래스도 받는다(생성 가능성은 생성 때 따로 본다 — UE TSubclassOf도 추상 클래스를 담는다).
// Deprecated 클래스는 이 판정이 받는다(decode는 진단, encode는 허용 — 같은 문서 5.2절). 쓰기는
// CheckWritableClassReferenceLease가 Deprecated를 거부한다(리플렉션 B9h2).
//
// 사용 예:
//   auto lease = detail::CheckClassReferenceLease(types.Acquire(value), schemaBase, nullptr);
//   if (!lease) { return Result<void>::Failure(lease.Error()); }
inline Result<TypeLease> CheckClassReferenceLease(Result<TypeLease> acquired, TypeId base,
                                                  const void* baseNative) noexcept {
    if (!acquired) {
        return Result<TypeLease>::Failure(acquired.Error() == ErrorCode::TypeMismatch
                                              ? ErrorCode::InvalidArgument
                                              : acquired.Error());
    }

    const TypeLease& lease = acquired.Value();
    if ((base != TypeId{} && !lease.IsA(base)) || !lease.IsNativeA(baseNative)) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    return acquired;
}

// ClassRef 쓰기(FieldValue 쓰기·staged 쓰기·경로 쓰기·경로 키)의 판정이다:
// CheckClassReferenceLease에 더해 Deprecated 클래스(상속 포함 — 리플렉션 B9h2)는
// InvalidArgument다(이미 저장된 값의 로드·저장은 받는다).
//
// 사용 예:
//   auto lease = detail::CheckWritableClassReferenceLease(registry.AcquireRegisteredType(access,
//                                                         value), schemaBase, nullptr);
inline Result<TypeLease> CheckWritableClassReferenceLease(Result<TypeLease> acquired, TypeId base,
                                                          const void* baseNative) noexcept {
    auto checked = CheckClassReferenceLease(Move(acquired), base, baseNative);
    if (checked && HasFlag(checked.Value().Schema().flags, TypeFlags::Deprecated)) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    return checked;
}

} // namespace detail

// 리플렉션 클래스 Base 또는 그 파생 클래스 하나를 가리키는 값이다(null 허용). PROPERTY로 두면 값
// 종류 ClassReference이고(클래스 필드·struct 멤버·Array 원소 — 1.3절 성질 표, Map 키·값·Set
// 원소·정적 배열 원소는 그 컨테이너 단계가 연다) 스키마 value.type은 Base의 TypeId다. Base는
// 리플렉션 클래스여야 한다(MetaCompiler RMC2003 — Object 자체는 Base가 될 수 없다). 저장 wire는
// Guid(클래스 TypeId, null은 0)다.
//
// 검사 시점(5.2절 표): 쓰기(FieldValue 쓰기·staged 쓰기·경로 쓰기)는 등록·클래스·Base 파생을 보고
// (미등록 NotFound, 그 밖 InvalidArgument), encode도 같은 검사로 C++가 직접 넣은 값을 막으며,
// 그래프 복원 1단계는 TypeRegistry 조회로 판정한다(미등록은 decode 정책 UnknownClassPolicy). 값
// 자체는 TypeRegistry를 붙잡지 않는다 — 클래스를 해제하면 값은 남고 Resolve가 NotFound다.
//
// Base가 불완전 타입이어도 선언할 수 있다(ClassRef<Self> 멤버 — 판정은 NativeTypeKey<Base>로 한다).
// 값 비교는 TypeId 같음이다.
//
// 사용 예:
//   ROCKS_PROPERTY(Required) ClassRef<Pawn> pawnClass;
//   pawnClass = ClassRef<Pawn>::Of<Knight>();          // Knight는 Pawn의 파생
//   auto lease = pawnClass.Resolve(types);             // 등록·클래스·Base 파생 재확인
template <class Base>
class ClassRef {
public:
    constexpr ClassRef() noexcept = default;

    // 클래스 TypeId로 만든다(검사하지 않는다 — 리플렉션 쓰기·저장이 검사한다). 0은 null이다.
    constexpr explicit ClassRef(TypeId type) noexcept : type_(type) {
    }

    // 리플렉션 클래스 T(Base 자신이거나 C++ 파생 — 컴파일 시 검사)의 값이다. T의 생성 헤더가 보여야
    // 한다(GeneratedType<T>::Id).
    template <class T>
    static ClassRef Of() noexcept {
        static_assert(IsBaseOfV<Base, T>, "ClassRef<Base>::Of<T> requires T to derive from Base");
        return ClassRef(TypeIdOf<T>());
    }

    // 클래스 TypeId다(null이면 0).
    constexpr TypeId Id() const noexcept {
        return type_;
    }

    constexpr bool IsNull() const noexcept {
        return type_ == TypeId{};
    }

    constexpr bool operator==(const ClassRef&) const noexcept = default;

    // 값이 types에 등록된 클래스이고 Base의 C++ 파생인지 다시 확인해 그 lease를 준다(Base 판정은
    // 등록 base 사슬의 native 키 — 같은 C++ 타입의 별칭 TypeId도 받는다). lease가 있는 동안 그
    // 클래스의 Unregister는 Busy다.
    //
    // 반환: 그 클래스의 lease. 실패 — null InvalidArgument, 미등록 NotFound, 클래스가 아니거나
    // Base의 파생이 아님 InvalidArgument, 종료 ShuttingDown, lease 포화 Overflow.
    Result<TypeLease> Resolve(TypeRegistry& types) const noexcept {
        static_assert(IsBaseOfV<Object, Base>, "ClassRef<Base> requires a reflected class Base");
        if (IsNull()) {
            return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
        }

        return detail::CheckClassReferenceLease(types.Acquire(type_), TypeId{},
                                                NativeTypeKey<Base>());
    }

private:
    TypeId type_{};
};

// 클래스 참조 키(리플렉션 Map 키·Set 원소 — OrderedMap<ClassRef<Base>, V>, Docs/Design/
// ReflectionExtensions.ko.md 7.1·7.2절)의 seed 해시다. 클래스 TypeId의 Guid 해시와 같다.
template <class Base>
struct SeededHash<ClassRef<Base>> : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    SeededHash() noexcept = default;

    uint64 operator()(const ClassRef<Base>& value) const noexcept {
        const uint64 words[2] = {value.Id().high, value.Id().low};
        return SipHash24Words(Seed(), words, 2);
    }
};

} // namespace rocks
