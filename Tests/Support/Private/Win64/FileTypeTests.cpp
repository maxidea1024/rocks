#include <Engine/PlatformBase/Private/Win64/FileType.h>
#include <Tests/Support/Test.h>

using namespace rocks;
using namespace rocks::platform::detail;

namespace {
uint32 simulatedType = 0;
uint32 simulatedError = 0;
uint32 phase = 0;
void ClearError() noexcept {
    phase = 1;
}
uint32 Query(void*) noexcept {
    if (phase != 1) {
        phase = 99;
    } else {
        phase = 2;
    }
    return simulatedType;
}
uint32 LastError() noexcept {
    if (phase != 2) {
        phase = 99;
    } else {
        phase = 3;
    }
    return simulatedError;
}
} // namespace

TEST(PlatformFileWindowsUnknownTypeDistinguishesSuccessAndFailure) {
    constexpr FileTypeOperations operations{ClearError, Query, LastError};
    simulatedType = 0;
    simulatedError = 0;
    auto result = QueryFileType(nullptr, operations);
    CHECK(result.type == 0 && result.error == 0 && phase == 3);
    simulatedError = 1234;
    result = QueryFileType(nullptr, operations);
    CHECK(result.type == 0 && result.error == 1234 && phase == 3);
    simulatedType = 1;
    simulatedError = 1234;
    result = QueryFileType(nullptr, operations);
    CHECK(result.type == 1 && result.error == 0 && phase == 2);
}
