#pragma once

#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace identity_fixture {

// 예전 직렬화 이름 OriginalActor::health를 도구로 변경한 뒤의 선언이다.
// 안정 식별자는 선언에 직접 적지 않고 명시적인 identity manifest에서 제공한다.
ROCKS_CLASS()
class RenamedActor final : public rocks::Object {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY()
    rocks::int32 hitPoints = 0;
};

} // namespace identity_fixture
