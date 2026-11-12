#pragma once

namespace rocks::platform {

// 복구할 수 없는 invariant 실패에 사용한다. 유효한 문자열 또는 nullptr를 받으며
// 최대 4096 byte의 stderr 출력을 시도한 뒤 abort한다. 일반 logger나 heap formatting은 없다.
// 정상 종료의 flush·자원 정리를 수행하지 않으며 출력의 완전한 저장도 보장하지 않는다.
[[noreturn]] void Fatal(const char* message) noexcept;

} // namespace rocks::platform
