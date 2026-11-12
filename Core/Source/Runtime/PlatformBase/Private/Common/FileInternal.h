#pragma once
#include <Base/Types.h>

namespace rocks::platform::detail {
// 경로로 전달할 UTF-8 scalar 및 NUL을 검사한다. 파일명 규칙이나 접근 권한은 검사하지 않는다.
bool ValidFilePath(const char* path, usize bytes) noexcept;
} // namespace rocks::platform::detail
