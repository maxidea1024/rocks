#include <Core/Error.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;

// 모든 열거자가 고유한 진단 이름을 가진다. 새 열거자를 끝에 더하고 이름을 빠뜨리면 "Unknown"으로
// 드러난다.
TEST(ErrorCodeNamesAreCompleteAndUnique) {
    constexpr uint32 last = static_cast<uint32>(ErrorCode::AuthenticationFailed);
    for (uint32 i = 0; i <= last; ++i) {
        const char* name = ErrorCodeName(static_cast<ErrorCode>(i));
        CHECK(strcmp(name, "Unknown") != 0);
        for (uint32 j = 0; j < i; ++j) {
            CHECK(strcmp(name, ErrorCodeName(static_cast<ErrorCode>(j))) != 0);
        }
    }

    CHECK(strcmp(ErrorCodeName(ErrorCode::AuthenticationFailed), "AuthenticationFailed") == 0);
    CHECK(strcmp(ErrorCodeName(static_cast<ErrorCode>(last + 1)), "Unknown") == 0);
}
