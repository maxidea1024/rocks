#include "CommandletHost.h"
#include <signal.h>
#include <stdio.h>

// macOS 표준 출력 채널이다(stdio에 UTF-8 byte 그대로 쓰고 매번 비운다).
namespace {

rocks::Result<void> Write(void*, rocks::CommandletStream stream, rocks::StringView text) noexcept {
    if (!text.ValidateUtf8())
        return rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument);
    FILE* output = stream == rocks::CommandletStream::Output ? stdout : stderr;
    if (text.ByteCount() != 0 &&
        fwrite(text.Data(), 1, text.ByteCount(), output) != text.ByteCount()) {
        return rocks::Result<void>::Failure(rocks::ErrorCode::DependencyFailed);
    }
    return fflush(output) == 0 ? rocks::Result<void>::Success()
                               : rocks::Result<void>::Failure(rocks::ErrorCode::DependencyFailed);
}

} // namespace

namespace rocks::commandlet_host {

CommandletOutput StandardOutput() noexcept {
    return {&Write, nullptr};
}

bool PrepareStandardOutput() noexcept {
    // 파이프 독자가 사라져도 OS 신호로 즉시 종료하지 않고 쓰기 오류를 반환한다.
    return signal(SIGPIPE, SIG_IGN) != SIG_ERR;
}

} // namespace rocks::commandlet_host
