#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>

// 글꼴 라이브러리 프로세스 전역 상태(TextLibraries.cpp)의 모듈 안 접근자다.
namespace rocks::text::detail {

// InitializeTextLibraries의 본체다.
Result<void> Initialize(Span<const uint8> icuData) noexcept;

// Initialize가 성공했으면 true다.
bool IsInitialized() noexcept;

} // namespace rocks::text::detail
