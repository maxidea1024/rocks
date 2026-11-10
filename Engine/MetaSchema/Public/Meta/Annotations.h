#pragma once

// Clang의 annotation payload를 host AST 도구가 읽는다. marker를 추가 인자로 반복하는 이유는
// Clang AST JSON이 annotation 본문 대신 추가 constant argument를 제공하기 때문이다.
// 지원하지 않는 compiler에서 조용히 annotation을 지우는 fallback을 제공하지 않는다.
#define ROCKS_REFLECT_TYPE(stableId, version)                                                      \
    [[clang::annotate("rocks", "rocks.type", stableId, version)]]
#define ROCKS_REFLECT_FIELD(stableId) [[clang::annotate("rocks", "rocks.field", stableId)]]
