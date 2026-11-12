#pragma once
#include <Base/Types.h>

namespace rocks::platform::detail {

// Windows의 FILE_TYPE_UNKNOWN은 유효한 unknown과 실패를 모두 나타낸다.
// 별도 호출 경계로 만들어 마지막 오류 초기화·캡처 순서를 테스트한다.
struct FileTypeOperations {
    void (*clearError)() noexcept;
    uint32 (*query)(void*) noexcept;
    uint32 (*lastError)() noexcept;
};

struct FileTypeResult {
    uint32 type;
    uint32 error;
};

inline FileTypeResult QueryFileType(void* handle, const FileTypeOperations& operations) noexcept {
    operations.clearError();
    const uint32 type = operations.query(handle);
    const uint32 error = type == 0 ? operations.lastError() : 0;
    return {type, error};
}

} // namespace rocks::platform::detail
