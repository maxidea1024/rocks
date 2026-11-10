#pragma once

#include <Meta/TypeSchema.h>

namespace rocks {

// World/PIE 인스턴스 등의 문맥 일치 확인용 값이다. 권한 인증 토큰은 아니다.
// 문맥이 일치해도 Registry 소유 스레드 검사를 별도로 통과해야 한다.
struct ObjectAccessContext {
    ContextId context;
};

} // namespace rocks
