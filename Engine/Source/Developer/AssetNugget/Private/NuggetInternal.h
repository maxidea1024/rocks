#pragma once

#include <Core/Containers/Span.h>

// AssetNugget 모듈 안 공용 도우미다.
namespace rocks::nugget_detail {

// byte 열 사전순 비교다(음수·0·양수). 공통 접두가 같으면 짧은 쪽이 먼저다. 키 기록의 정규 인코딩은
// 태그·길이 접두가 있어 prefix-free이고(한 인코딩이 다른 인코딩의 접두일 수 없다) Record의 비교
// 대상은 같은 길이라, 모듈 안 호출에서는 접두 갈래가 생기지 않는다 — 일반 규칙으로 둔다.
inline int32 CompareBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    const usize shared = left.Size() < right.Size() ? left.Size() : right.Size();
    for (usize index = 0; index < shared; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }

    if (left.Size() == right.Size()) {
        return 0;
    }

    return left.Size() < right.Size() ? -1 : 1;
}

} // namespace rocks::nugget_detail
