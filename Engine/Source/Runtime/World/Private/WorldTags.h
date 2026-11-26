#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>

namespace rocks {
class ObjectRegistry;
} // namespace rocks

// Actor·Component 태그 값 도우미다(Docs/Design/EngineRuntime 31.4절 — WorldTags.cpp에 정의).
namespace rocks::world_tags {

// tags에 tag가 있는지다(None은 거짓).
bool Contains(Span<const Name> tags, Name tag) noexcept;

// tag가 registry의 이름 pool에서 발급한 None 아닌 토큰인지 본다. None·다른 pool·옛 토큰은
// InvalidArgument, pool 없음은 Unsupported, 그 밖의 Acquire 오류는 그대로다.
Result<void> CheckToken(ObjectRegistry& registry, Name tag) noexcept;

// 검사 뒤 없으면 끝에 더한다(있으면 그대로 성공). 실패하면 tags는 그대로다.
Result<void> Add(Array<Name>& tags, ObjectRegistry& registry, Name tag) noexcept;

// 같은 tag를 모두 뺀다(순서 유지). None은 InvalidArgument다.
Result<void> Remove(Array<Name>& tags, Name tag) noexcept;

// 원소마다 검사한 뒤 통째로 바꾼다. 실패하면 tags는 그대로다.
Result<void> Set(Array<Name>& tags, ObjectRegistry& registry, Span<const Name> values) noexcept;

} // namespace rocks::world_tags
