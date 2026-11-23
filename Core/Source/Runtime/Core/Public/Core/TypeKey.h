#pragma once

#include <Base/Types.h>

// 정확한 C++ 타입 비교용 실행 중 키다(Docs/Design/BuildTargets 3.8절 BS8d4·BS8e). 저장하거나
// 직렬화하지 않는다. 타입이 서로 다른 모듈에서 같은 키를 가져야 하면(Modular — 모듈마다 공유
// 라이브러리) 그 타입의 공개 헤더에 ROCKS_DECLARE_NATIVE_TYPE_KEY를 두고 그 모듈의 .cpp 하나에
// ROCKS_DEFINE_NATIVE_TYPE_KEY로 정의한다. 선언하지 않은 타입의 키는 inline 표지의 주소라 실행
// 이미지마다 다르다 — 한 모듈 안에서만 쓰는 타입(시험·지역 타입·기본 타입)은 그것으로 충분하다.
// 리플렉션 타입의 키는 ObjectRuntime NativeTypeKey가 생성 키로 준다(Object/Object.h).
//
// 타입의 주소를 키로 쓰는 다른 표지(헤더의 변수 템플릿 주소)는 두지 않는다 — 정책
// public-header-type-tag가 막는다. 이 헤더의 TypeKeyOf를 쓴다.
//
// 사용 예:
//   const void* key = TypeKeyOf<Spinner>();
//   // Spinner.h — 다른 모듈도 같은 키를 쓸 때
//   ROCKS_DECLARE_NATIVE_TYPE_KEY(game::Spinner, ROCKS_GAME_API)
//   // Spinner.cpp
//   ROCKS_DEFINE_NATIVE_TYPE_KEY(game::Spinner)
namespace rocks {

// 선언한 export 키가 없는 타입의 표지다. 타입마다 고유한 주소다. const를 붙이지 않는다: 읽기 전용
// 상수는 링커의 동일 데이터 병합(ICF)으로 서로 다른 타입의 주소가 합쳐질 수 있다. inline 변수라
// Modular에서는 공유 라이브러리마다 따로 생긴다.
template <class T>
inline uint8 NativeTypeIdentity = 0;

// export 키 선언 자리다(ROCKS_DECLARE_NATIVE_TYPE_KEY가 특수화한다).
template <class T>
struct NativeTypeKeyTraits;

// T의 키다: 선언한 export 키가 있으면 그것, 없으면 NativeTypeIdentity<T>의 주소다. 선언이
// 보이는지는 템플릿 인자(Keyed)의 기본값으로 정한다 — 본문 안에서 판정하면 선언이 보이는 번역
// 단위와 보이지 않는 번역 단위의 같은 이름 inline 정의가 둘이 되어(ODR 위반) 링커가 고른 정의에
// 따라 키가 바뀐다.
template <class T, bool Keyed = requires { NativeTypeKeyTraits<T>::Key(); }>
const void* TypeKeyOf() noexcept {
    if constexpr (Keyed) {
        return NativeTypeKeyTraits<T>::Key();
    } else {
        return &NativeTypeIdentity<T>;
    }
}

} // namespace rocks

// T의 export 키를 선언한다. 타입의 공개 헤더에서 타입 정의 뒤 전역 namespace에 쓰고, Api는 그
// 모듈의 API 매크로다.
#define ROCKS_DECLARE_NATIVE_TYPE_KEY(Type, Api)                                                   \
    template <>                                                                                    \
    struct rocks::NativeTypeKeyTraits<Type> {                                                      \
        Api static const void* Key() noexcept;                                                     \
    };

// T의 export 키를 정의한다. 그 모듈의 .cpp 하나에 전역 namespace로 쓴다.
#define ROCKS_DEFINE_NATIVE_TYPE_KEY(Type)                                                         \
    const void* rocks::NativeTypeKeyTraits<Type>::Key() noexcept {                                 \
        static unsigned char key = 0;                                                              \
        return &key;                                                                               \
    }
