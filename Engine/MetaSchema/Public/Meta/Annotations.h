#pragma once

#include <Meta/GeneratedType.h>

// Clang JSON AST가 추가 constant argument를 제공하므로 식별 marker를 인자로 반복한다.
// specifier는 제한 문법의 annotation payload이며 C++ 선언은 Clang AST만으로 해석한다.
// 지원하지 않는 compiler에서 annotation을 지우는 fallback은 제공하지 않는다.
// 이 마커는 바로 뒤 class/struct와 같은 소스·스코프에 위치해야 한다.
// 이름은 번역 단위에서만 유일하면 되며 정의된 enum 상수는 실행 코드나 저장 공간을 만들지 않는다.
#define ROCKS_DETAIL_JOIN_INNER(left, right) left##right
#define ROCKS_DETAIL_JOIN(left, right) ROCKS_DETAIL_JOIN_INNER(left, right)
#define ROCKS_CLASS(stableId, version, ...)                                                        \
    enum {                                                                                         \
        ROCKS_DETAIL_JOIN(rocksMetaClassMarker_, __COUNTER__)                                      \
        [[clang::annotate("rocks", "rocks.class.marker", stableId, version, #__VA_ARGS__)]] = 0    \
    };
#define ROCKS_PROPERTY(stableId, ...)                                                              \
    [[clang::annotate("rocks", "rocks.field", stableId, #__VA_ARGS__)]]

// generated typed 접근기만 private/protected 필드 접근을 허용한다. 파생 타입에 펼친 기반 필드도
// 해당 GeneratedType specialization을 통해 접근한다. raw 주소 공개나 Object 수명 검사를 우회하지
// 않는다.
#define ROCKS_GENERATED_BODY()                                                                     \
    template <class>                                                                               \
    friend struct ::rocks::GeneratedType;                                                          \
    [[clang::annotate("rocks", "rocks.generated")]] static void                                    \
    rocksGeneratedAccessMarker() noexcept;

// 두 이름 모두 class/struct 앞에 둔다. 두 인자만 쓰는 기존 이름은 동일한 마커의 별칭이다.
#define ROCKS_REFLECT_TYPE(stableId, version) ROCKS_CLASS(stableId, version)
#define ROCKS_REFLECT_FIELD(stableId) ROCKS_PROPERTY(stableId)
