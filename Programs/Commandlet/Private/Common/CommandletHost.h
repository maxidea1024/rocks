#pragma once

#include <Core/Commandlet/Commandlet.h>

namespace rocks::commandlet_host {

// OS entrypoint가 인수와 출력 채널을 소유하며 이 함수는 실행 중에만 빌린다.
int Run(Span<const StringView> args, CommandletOutput output) noexcept;

} // namespace rocks::commandlet_host
