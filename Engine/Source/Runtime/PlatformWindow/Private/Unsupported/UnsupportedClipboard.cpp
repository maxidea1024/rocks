#include <PlatformWindow/Clipboard.h>

namespace rocks::platform {

Result<void> ReadClipboardText(String&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> WriteClipboardText(StringView) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::platform
