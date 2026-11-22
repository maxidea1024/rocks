#pragma once

#include <Base/TypeTraits.h>
#include <Core/Result.h>
#include <Core/TypeKey.h>
#include <Meta/GeneratedType.h>
#include <Object/BoundCoreValues.h>
#include <Object/ObjectHandle.h>
#include <Object/TypeLease.h>

namespace rocks {

class ObjectRegistry;
class ConstructionContext;
class TraceVisitor;

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

private:
    friend class ObjectRegistry;
    // 생성자 반환 후 설정하며 물리 소멸까지 변경하지 않는다. Registry가 객체보다 오래 산다.
    ObjectRegistry* registry_ = nullptr;
    ObjectHandle handle_;
};

// 정확한 C++ 타입 비교용 키다. 직렬화하거나 ABI에 노출하지 않는다. Object의 nullptr 키는 기반 타입
// 조회를 뜻한다. 상속 판정은 등록된 base 사슬을 따라 TypeLease::IsNativeA가 한다. 키는 소유 모듈에
// 하나라 공유 라이브러리 사이에서도 같다(Docs/Design/BuildTargets.ko.md 3.8절 BS8b·BS8d4).
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
