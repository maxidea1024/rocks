#pragma once

#include <Object/TypeRegistry.h>

// 메타컴파일러를 거치지 않는 네이티브 클래스의 타입 등록이다. 메타 생성 타입은 모든 필드가 반영
// 가능한 형식이어야 하지만(컨테이너·raw pointer 거부), 엔진 런타임 클래스(Actor·Component 등)는
// Array, World 포인터 같은 C++ 상태가 필요하다. 이런 클래스는 필드 없는 스키마로 등록해 Object
// 생성·수명· 상속 판정(Resolve<기반 타입>)만 쓴다. 필드 리플렉션·객체 그래프 직렬화 대상은 아니다.
//
// 사용 예:
//   class SpinningActor : public rocks::Actor {
//       ROCKS_NATIVE_CLASS(SpinningActor, rocks::Actor, 0x5b1c7d2e8a4f4c10ull,
//       0x9e02b6a1d3c84f57ull)
//   public:
//       explicit SpinningActor(rocks::Allocator& allocator) noexcept;
//   };
//
//   // 기반 타입을 먼저 등록한다(TypeRegistry 규칙). 실패하면 AlreadyExists·NotFound 등을 반환한다.
//   auto token = rocks::RegisterNativeClass<SpinningActor>(types);
//
// - TypeId는 저장소 전체에서 유일한 128비트 값이다. 새 클래스마다 무작위 값을 발급한다(재사용
// 금지).
// - 매크로는 `public:` 접근 지정을 남긴다. 매크로 뒤의 멤버 접근 지정은 직접 다시 쓴다.
// - 기반 클래스도 네이티브 클래스이거나 rocks::Object여야 한다. 단일 public 비가상 상속만 지원한다.
// - 파생 클래스가 매크로를 빠뜨리면 기반의 ID를 물려받는다. RegisterNativeClass가 컴파일 시
// 거부한다.
#define ROCKS_NATIVE_CLASS(Type, Base, IdHigh, IdLow)                                              \
public:                                                                                            \
    using ThisClass = Type;                                                                        \
    using Super = Base;                                                                            \
    static constexpr ::rocks::TypeId StaticTypeId{IdHigh, IdLow};                                  \
    static constexpr char StaticTypeName[] = #Type;

namespace rocks {

// ROCKS_NATIVE_CLASS를 선언한 Object 파생 클래스다.
template <class T>
concept NativeClassType = __is_base_of(Object, T) && requires {
    typename T::ThisClass;
    typename T::Super;
    T::StaticTypeId;
    T::StaticTypeName;
};

// T의 정적 TypeId다. ROCKS_NATIVE_CLASS 선언이 필요하다. 등록 여부와 무관한 값이다.
template <NativeClassType T>
constexpr TypeId NativeTypeIdOf() noexcept {
    return T::StaticTypeId;
}

// T를 필드 없는 스키마로 등록한다. 성공하면 등록 토큰을 반환하며, 이 토큰으로 Unregister할 수 있다.
// 기반 타입이 아직 등록되지 않았으면 NotFound, 같은 ID가 이미 있으면 AlreadyExists, 종료 중이면
// ShuttingDown이다. 실패해도 기존 등록 상태는 바뀌지 않는다. 등록 스레드 규약은 TypeRegistry를
// 따른다. module은 동적 모듈의 코드 수명을 유지할 때 넘긴다(정적 링크면 생략).
template <NativeClassType T>
Result<TypeRegistrationToken> RegisterNativeClass(TypeRegistry& types,
                                                  ModuleLease module = {}) noexcept {
    static_assert(__is_same(typename T::ThisClass, T),
                  "ROCKS_NATIVE_CLASS is missing in this class; it inherited its base's identity");
    using Base = typename T::Super;
    static_assert(__is_base_of(Base, T) && !__is_same(Base, T));
    static_assert(__is_same(Base, Object) || NativeClassType<Base>,
                  "native class base must be rocks::Object or another native class");

    TypeSchema schema;
    schema.id = T::StaticTypeId;
    schema.name = Span<const char>(T::StaticTypeName, sizeof(T::StaticTypeName) - 1);
    schema.flags = __is_abstract(T) ? static_cast<uint32>(TypeFlags::Abstract) : 0u;
    RuntimeTypeOps ops = MakeRuntimeTypeOps<T>(Move(module));
    if constexpr (!__is_same(Base, Object)) {
        schema.baseType = Base::StaticTypeId;
        ops.nativeBaseType = NativeTypeKey<Base>();
    }

    return types.Register(schema, Move(ops));
}

} // namespace rocks
