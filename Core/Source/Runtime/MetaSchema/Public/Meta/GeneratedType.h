#pragma once

#include <Base/TypeTraits.h>
#include <Meta/TypeSchema.h>

namespace rocks {

// MetaCompiler 출력이 구체 타입마다 특수화한다. 수동 field offset이나 암묵 fallback은 없다.
// 생성된 Schema/Ops/Register를 사용하며 include되지 않은 타입은 불완전 타입으로 남는다.
template <class T>
struct GeneratedType;

namespace detail {

// ROCKS_GENERATED_BODY()가 타입에 남긴 표지를 읽는다. 매크로가 이 struct를 friend로 두므로 표지가
// private 구역에 있어도 보인다. 표지는 자기 타입의 포인터를 돌려주는 멤버 함수 선언이어서, 기반
// 클래스에서 물려받은 표지는 반환 타입이 달라 세지 않는다.
struct ReflectedDeclarationProbe {
    template <class T>
    static constexpr bool Own =
        requires(T& value) { requires IsSameV<decltype(value.rocksReflectedSelf()), T*>; };
};

} // namespace detail

// T가 리플렉션 클래스·struct로 선언됐는지다(정의 안의 ROCKS_GENERATED_BODY()). 타입의 선언만으로
// 정해지므로 생성 헤더를 include했는지와 무관하게 번역 단위마다 같은 값이다
// (Docs/Design/ReflectionGeneratedSources.ko.md 8.1절). enum과 Binds 미러의 실제 타입에는 표지가
// 없다. T는 완전한 타입이어야 한다(전방 선언만 보이면 거짓이다).
template <class T>
inline constexpr bool ReflectedDeclarationV =
    IsClassV<T> && detail::ReflectedDeclarationProbe::Own<T>;

namespace detail {

// GeneratedType<T>의 유무로 길을 고르는 템플릿이 거치는 관문이다. T의 생성 헤더가 이 번역 단위에
// 보이면 참이다. 리플렉션 타입으로 선언됐는데 보이지 않으면 컴파일 오류다 — 탐지식이 조용히 거짓이
// 되어 "리플렉션 타입이 아닌 경우"의 길로 가는 것을 막는다(같은 템플릿이 번역 단위마다 다르게
// 인스턴스화되는 ODR 위반이기도 하다).
//
// 사용 예:
//   static constexpr bool Supported =
//       IsClassV<T> && detail::GeneratedHeaderVisible<T>() && requires { GeneratedType<T>::Id(); };
template <class T>
constexpr bool GeneratedHeaderVisible() noexcept {
    static_assert(
        !ReflectedDeclarationV<T> || requires { sizeof(GeneratedType<T>); },
        "this type is reflected (ROCKS_GENERATED_BODY) but its generated header is not included "
        "in this translation unit; include <Module/Header.generated.h> before this use");
    return requires { sizeof(GeneratedType<T>); };
}

} // namespace detail

// 사용 예: TypeIdOf<MyActor>(). 해당 타입의 MetaCompiler 생성 헤더를 먼저 포함해야 한다.
// ID는 값으로 복사되며 등록 여부와 관계없다. 이름 변경·영속성은 별도의 identity 규약을 따른다.
template <class T>
constexpr TypeId TypeIdOf() noexcept {
    if constexpr (requires { GeneratedType<T>::Id(); }) {
        return GeneratedType<T>::Id();
    } else {
        static_assert(
            requires { GeneratedType<T>::Id(); },
            "TypeIdOf requires a reflected type and its generated header");
        return {};
    }
}

// 등록 전 생성 스키마다. 반환 참조와 필드 이름은 생성 코드가 들어 있는 모듈이 살아 있을 때만
// 유효하다. 등록된 메타데이터를 오래 보관하려면 TypeRegistry의 TypeLease를 사용한다.
template <class T>
const TypeSchema& TypeOf() noexcept {
    if constexpr (requires { GeneratedType<T>::Schema(); }) {
        return GeneratedType<T>::Schema();
    } else {
        static_assert(
            requires { GeneratedType<T>::Schema(); },
            "TypeOf requires a reflected type and its generated header");
        platform::Fatal("TypeOf called without generated metadata");
    }
}

} // namespace rocks
