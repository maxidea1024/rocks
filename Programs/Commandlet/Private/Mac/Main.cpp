#include "CommandletHost.h"
#include <signal.h>
#include <stdio.h>

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

int main(int argc, char** argv) {
    // 파이프 독자가 사라져도 OS 신호로 즉시 종료하지 않고 쓰기 오류를 반환한다.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return 1;
    if (argc < 1 || argv == nullptr ||
        argc - 1 > static_cast<int>(rocks::CommandletRegistry::MaxArguments)) {
        return Write(nullptr, rocks::CommandletStream::Error, "Too many or invalid arguments.\n")
                   ? 2
                   : 1;
    }
    rocks::StringView args[rocks::CommandletRegistry::MaxArguments];
    for (int index = 1; index < argc; ++index) {
        if (!argv[index])
            return Write(nullptr, rocks::CommandletStream::Error, "Invalid argument.\n") ? 2 : 1;
        args[index - 1] = rocks::StringView(argv[index]);
        if (args[index - 1].ByteCount() > rocks::CommandletRegistry::MaxArgumentBytes ||
            !args[index - 1].ValidateUtf8()) {
            return Write(nullptr, rocks::CommandletStream::Error,
                         "Invalid or oversized UTF-8 argument.\n")
                       ? 2
                       : 1;
        }
    }
    return rocks::commandlet_host::Run({args, static_cast<rocks::usize>(argc - 1)},
                                       {&Write, nullptr});
}
