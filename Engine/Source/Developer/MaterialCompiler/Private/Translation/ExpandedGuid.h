#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>

// 펼친 그래프의 id hash다(Docs/Design/MaterialSystem 6.12절 MS2e2·MS2e3): parts의 SHA-256 앞 128
// bit(각 Guid는 high·low를 little endian 8 byte씩, 결과는 digest 앞 8 byte가 high). 함수 사본 id,
// 파라미터 정체, 레이어 합성 호출 id가 쓴다.
namespace rocks::material::detail {

Guid DeriveExpandedGuid(Span<const Guid> parts) noexcept;

} // namespace rocks::material::detail
