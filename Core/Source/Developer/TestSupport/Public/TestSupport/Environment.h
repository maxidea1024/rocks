#pragma once

#include <Core/Text/StringBuilder.h>

namespace rocks::test {

// 프로세스 환경 변수 name(ASCII)의 값을 UTF-8로 out 끝에 붙인다. 외부 입력(UE가 내보낸 묶음 같은
// 저장소 밖 파일)을 받는 선택 시험이 쓴다. 변수가 없으면 false이고 out은 그대로다. 값을 UTF-8로
// 바꾸지 못하거나 붙이지 못해도 false다. 빈 값은 true(아무것도 붙이지 않음)다.
//
// 사용 예:
//   StringBuilder<512> directory(allocator);
//   if (!test::ReadEnvironmentVariable("ROCKS_UNREAL_EXCHANGE_BUNDLE", directory)) { return; } //
//   SKIP
bool ReadEnvironmentVariable(const char* name, StringBuilderBase& out) noexcept;

} // namespace rocks::test
