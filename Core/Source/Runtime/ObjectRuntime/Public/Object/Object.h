#pragma once

#include <Base/TypeTraits.h>
#include <Core/Result.h>
#include <Core/TypeKey.h>
#include <Meta/Annotations.h>
#include <Meta/GeneratedType.h>
#include <Object/BoundCoreValues.h>
#include <Object/ObjectHandle.h>
#include <Object/TypeLease.h>

namespace rocks {

class ObjectRegistry;
class ConstructionContext;
class TraceVisitor;
class Object;

namespace detail {

// 멤버 포인터 형식의 소유 클래스·멤버 형식이다(멤버 함수 포인터면 Value는 함수 형식이다).
template <class T>
struct MemberPointerParts;

template <class M, class C>
struct MemberPointerParts<M C::*> {
    using Owner = C;
    using Value = M;
};

// 코드 통지·생성 setter의 본체다(Object/ObjectSetter.h — 생성 헤더가 include한다). 이 헤더에서는
// 선언만 하므로 정의가 보이지 않는 번역 단위에서 NotifyChanged·SetAndNotify를 쓰면 불완전 형식
// 컴파일 오류다.
template <class MemberPointer>
struct SetterAccess;

} // namespace detail

// 레지스트리에서 관찰한 한 시점의 논리 상태다. 값은 객체 수명이나 접근권을 보존하지 않는다.
// Unreachable은 증분 GC cutoff에서 아직 Retired로 분류되지 않은 white 객체다.
// Retired는 명시 파괴와 GC 회수 중 어느 경로인지 구별하지 않는다.
enum class ObjectLifecycle : uint8 {
    Alive,
    DestroyRequested,
    Unreachable,
    Retired
};

// Registry가 식별자와 저장소를 소유하는 기반 타입. 직접 delete하지 않는다.
// 복사·이동은 객체 정체성과 ObjectRef 소유자 검사를 우회하므로 금지한다.
// 파생 타입의 정확한 소멸자는 RuntimeTypeOps를 통해 호출한다.
class ROCKS_OBJECTRUNTIME_API Object {
public:
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&&) = delete;
    Object& operator=(Object&&) = delete;

    ObjectHandle Handle() const noexcept {
        return handle_;
    }

    // 소유 스레드와 이 객체의 활성 Borrow가 필요하다. 실행 scope가 있으면 해당 문맥을 검사하고,
    // 없으면 기본 설정에서는 이 객체의 Registry 문맥을 사용한다. Registry가 엄격 모드라면 scope
    // 누락은 실패한다. scope가 없을 때 호출자의 World는 검증할 수 없다. 생성자 안과 미공개
    // 상태는 실패한다. 같은 Borrow 안에서 이미 요청된 파괴의 재요청은 성공한다. 패키지 정책
    // Registry(RegistryPolicy::packageNativeType)의 객체는 접근 검사 다음에 언제나 Unsupported다
    // (재요청 포함 — 소속 객체는 ObjectRegistry::Discard로만 죽는다).
    Result<void> Destroy() noexcept;

    // 실제 객체에 등록된 타입을 조회한다. 받은 lease는 스키마와 모듈 코드를 유지하며,
    // lease에서 빌린 참조는 lease보다 오래 보관하지 않는다. Object 자체의 수명은 연장하지 않는다.
    // 유효한 Borrow와 소유 스레드가 필요하다. 엄격 scope 설정도 Destroy와 동일하게 적용한다.
    // 오류 발생 시 GetType은 원인을 진단하고 종료한다.
    // 오류를 직접 처리하려면 TryGetType을 사용한다. 파괴 요청 후 새 타입 조회는 허용하지 않는다.
    [[nodiscard]] TypeLease GetType() const noexcept;
    [[nodiscard]] Result<TypeLease> TryGetType() const noexcept;

    // snapshot은 수명이나 접근권을 보존하지 않는다. 논리 파괴 이후에도 같은 Borrow가 살아 있는
    // 동안 이 함수와 IsDestroyRequested/TryIsDestroyRequested/중복 Destroy만 사용할 수 있다.
    // 다른 객체 접근은 금지다.
    Result<ObjectLifecycle> InspectLifecycle() const noexcept;
    // 유효한 Borrow 안에서 호출한다. 조회 실패를 상태로 오인하지 않도록 모든 빌드에서 Fatal이다.
    // 오류를 처리해야 한다면 TryIsDestroyRequested를 사용한다.
    [[nodiscard]] bool IsDestroyRequested() const noexcept;

    // 성공할 때만 requested를 갱신한다. cutoff/retired에서는 요청 원인을 알 수 없어 실패한다.
    [[nodiscard]] Result<void> TryIsDestroyRequested(bool& requested) const noexcept;

protected:
    Object() noexcept = default;
    ~Object() = default;

    // Member(이 객체 클래스나 그 기반의 Notify 필드·Notify 계산 값 함수 — &Class::member)가
    // 바뀌었을 수 있음을 구독자에게 알린다(UE BroadcastFieldValueChanged —
    // Docs/Design/ChangeNotification.ko.md CN-D9). 생성 setter가 없는 필드를 코드가 직접 바꿨을 때,
    // 계산 값 함수의 입력이 바뀌었을 때 쓴다. Member는 멤버 포인터 템플릿 인자이고 그 클래스의 생성
    // 헤더(GeneratedType<C>::NotifyId)가 ID로 바꾼다 — Notify 대상이 아니면 컴파일 오류다. bitfield
    // 필드는 쓸 수 없다(멤버 포인터가 없다). 부르는 코드는 그 클래스의 생성 헤더를 include한 .cpp에
    // 둔다(본체 detail::SetterAccess와 NotifyId가 생성 헤더로 보인다 — 리플렉션 헤더의 인라인
    // 본문은 메타 단계 Clang 실행이 생성 헤더 없이 읽으므로 불완전 형식 오류다).
    //
    // 조건: 소유 스레드, 이 객체의 접근 문맥(실행 scope, 없으면 Registry 기본 문맥 — 엄격 모드면
    // scope 누락은 실패). Borrow는 필요 없다(호출자가 이 객체의 멤버 함수 안에 있다). Member는 호출
    // 클래스나 그 기반의 멤버여야 한다(컴파일 검사). 재정의한 virtual 계산 값 함수는 선언한 기반의
    // 이름
    // (&Base::Function)으로 쓴다. 생성자 안(Registry 없음)·게시 전 객체·구독자 없는 객체는 아무것도
    // 하지 않고 Success다.
    // 오류: 접근 문맥·스레드(WrongContext·WrongThread 등 Registry 접근 검사의 오류), Registry 종료
    // ShuttingDown, 처리 표시·묶음 게시 구간 Busy.
    // 수명: 구독자는 이 호출 안에서 동기로 불린다(CN-D6).
    //
    // 사용 예:
    //   void Health::Damage(int32 amount) noexcept {
    //       current -= amount;
    //       (void)NotifyChanged<&Health::current>();
    //       (void)NotifyChanged<&Health::Percent>();   // ROCKS_FUNCTION(Pure, Notify) 계산 값
    //   }
    template <auto Member, class Self>
    Result<void> NotifyChanged(this Self& self) noexcept {
        static_assert(IsBaseOfV<typename detail::MemberPointerParts<decltype(Member)>::Owner, Self>,
                      "NotifyChanged needs a member of the calling class or one of its bases");
        static_assert(!IsConstV<Self>, "NotifyChanged needs a non-const object (call it from a "
                                       "non-const member function)");
        return detail::SetterAccess<decltype(Member)>::template Notify<Member>(self);
    }

    // Member(Notify 필드 — &Class::member)에 value를 넣고 알린다(UE UE_MVVM_SET_PROPERTY_VALUE —
    // CN-D9). 지금 값과 같으면(CN-D15 — Object/ValueSame.h) 아무것도 하지 않고 false, 넣었으면 알린
    // 뒤 true다. 대입 규칙은 생성 setter와 같다(Meta/Annotations.h ROCKS_SETTER — 할당하는 값은
    // 사본을 먼저 만들고 교환, 실패하면 그 오류이고 필드는 그대로). 생성 setter(ROCKS_SETTER)가
    // 있는 필드는 쓸 수 없다(컴파일 오류 — setter가 Apply까지 한다). 오류: 접근
    // 문맥·스레드·Registry 종료는 대입 전에 본다(필드 그대로). 사본 할당 실패(OutOfMemory 등), 사본
    // allocator가 없음(생성자 안의 Text·struct 등 — Unsupported)도 필드 그대로다. 대입 뒤 통지의
    // Busy(처리 표시·묶음 게시 구간)는 필드가 이미 새 값이다.
    //
    // 사용 예:
    //   if (auto changed = SetAndNotify<&Health::current>(next); changed && changed.Value()) {
    //       ...   // 바뀌었다
    //   }
    template <auto Member, class Self>
    Result<bool>
    SetAndNotify(this Self& self,
                 detail::SetterParam<typename detail::MemberPointerParts<decltype(Member)>::Value>
                     value) noexcept {
        static_assert(IsBaseOfV<typename detail::MemberPointerParts<decltype(Member)>::Owner, Self>,
                      "SetAndNotify needs a member of the calling class or one of its bases");
        static_assert(!IsConstV<Self>, "SetAndNotify needs a non-const object (call it from a "
                                       "non-const member function)");
        return detail::SetterAccess<decltype(Member)>::template SetAndNotify<Member>(self, value);
    }

private:
    friend class ObjectRegistry;
    template <class>
    friend struct detail::SetterAccess;

    // 이 객체의 통지 진입점이다(ObjectRegistry::NotifySelf — Registry가 없으면 Success).
    Result<void> NotifySelf(Guid id) noexcept;

    // 통지할 쓰기의 사전 검사다(ObjectRegistry::CheckSelfAccess — Registry가 없으면 Success): 대입
    // 전에 접근 문맥·스레드·종료를 보아 대입 뒤 실패를 Busy만 남긴다.
    Result<void> CheckNotifyAccess() const noexcept;

    // setter 사본의 allocator다: Registry의 객체 allocator, Registry가 없으면(생성자 안) nullptr.
    Allocator* ValueAllocator() const noexcept;

    // 생성자 반환 후 설정하며 물리 소멸까지 변경하지 않는다. Registry가 객체보다 오래 산다.
    ObjectRegistry* registry_ = nullptr;
    ObjectHandle handle_;
};

// 정확한 C++ 타입 비교용 키다. 직렬화하거나 ABI에 노출하지 않는다. Object의 nullptr 키는 기반 타입
// 조회를 뜻한다. 상속 판정은 등록된 base 사슬을 따라 TypeLease::IsNativeA가 한다. 키는 소유 모듈에
// 하나라 공유 라이브러리 사이에서도 같다(Docs/Design/BuildTargets 3.8절 BS8b·BS8d4).
// - 리플렉션 class·struct·enum과 Binds 미러의 실제 Core 값 타입(BoundCoreValueV)은 생성 .cpp가
//   정의한 저장소의 주소다(GeneratedType<T>::NativeKey — UE StaticStruct·TBaseStructure와 같은
//   자리). 그래서 그 타입의 생성 헤더를 include한 번역 단위에서만 쓸 수 있다. class·struct·Binds
//   대상은 없으면 컴파일 오류다(번역 단위마다 다른 키를 막는다). enum은 본문 표지를 둘 수 없어
//   판정하지 못한다: 생성 헤더 없이 리플렉션 enum의 키를 쓰면 native 키가 되어 조회가 NotFound로
//   실패한다(리플렉션하지 않는 enum도 native 등록 타입으로 쓸 수 있다).
// - native 등록 타입은 TypeKeyOf<T>()다(Core/TypeKey.h — ROCKS_DECLARE_NATIVE_TYPE_KEY가 있으면 그
//   export 키, 없으면 실행 이미지마다 다른 inline 표지의 주소).
// 생성 키·키 매크로가 보이는지는 템플릿 인자(Generated·Keyed)의 기본값으로 정한다. 그래서 그것이
// 보이는 번역 단위와 보이지 않는 번역 단위가 서로 다른 특수화를 쓴다 — 본문 안에서 판정하면 같은
// 이름의 inline 정의가 둘이 되어(ODR 위반) 링커가 고른 정의에 따라 등록 키까지 바뀔 수 있다.
// class 타입은 완전한 타입이어야 한다(전방 선언만 보이면 리플렉션 판정이 거짓이 되어 조용히
// 다른 키가 되므로 컴파일 오류).
//
// 사용 예:
//   #include <World/Actor.generated.h>
//   const void* key = NativeTypeKey<Actor>();
template <class T, bool Generated = requires { GeneratedType<T>::NativeKey(); },
          bool Keyed = requires { NativeTypeKeyTraits<T>::Key(); }>
const void* NativeTypeKey() noexcept {
    static_assert(
        !IsClassV<T> || requires { sizeof(T); },
        "NativeTypeKey needs a complete type: include the definition of T in this "
        "translation unit");
    if constexpr (IsSameV<T, Object>) {
        return nullptr;
    } else if constexpr (Generated) {
        return GeneratedType<T>::NativeKey();
    } else if constexpr (ReflectedDeclarationV<T> || BoundCoreValueV<RemoveCvT<T>>) {
        static_assert(
            !ReflectedDeclarationV<T> && !BoundCoreValueV<RemoveCvT<T>>,
            "NativeTypeKey of a reflected type (class, struct or a Binds target of "
            "CoreTypeBindings) needs its generated header: include <Module/Header.generated.h> "
            "in this translation unit");
        return nullptr;
    } else if constexpr (Keyed) {
        return NativeTypeKeyTraits<T>::Key();
    } else {
        return &NativeTypeIdentity<T>;
    }
}

} // namespace rocks
