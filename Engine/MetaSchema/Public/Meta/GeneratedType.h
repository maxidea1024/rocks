#pragma once

#include <Meta/TypeSchema.h>

namespace rocks {

// MetaCompiler 출력이 구체 타입마다 특수화한다. 수동 field offset이나 암묵 fallback은 없다.
// 생성된 Schema/Ops/Register를 사용하며 include되지 않은 타입은 불완전 타입으로 남는다.
template <class T>
struct GeneratedType;

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
