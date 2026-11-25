#include "DemoFiles.h"

#include <HostConfig/HostSettings.h>
#include <Platform/File.h>

namespace rocks::ui {

namespace {

// 경로 버퍼 byte 수다.
constexpr usize PathBytes = 1024;

} // namespace

Result<void> ReadEngineFile(StringView relative, Array<uint8>& out) noexcept {
    char engine[PathBytes] = {};
    auto found = FindEngineDirectory(Span<char>(engine, sizeof(engine)));
    if (!found) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const StringView base(engine, found.Value());
    if (base.ByteCount() + 1 + relative.ByteCount() + 1 > PathBytes) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    char path[PathBytes] = {};
    usize length = 0;
    for (char byte : base) {
        path[length++] = byte;
    }

    path[length++] = '/';
    for (char byte : relative) {
        path[length++] = byte;
    }

    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size)) &&
              static_cast<bool>(out.TryResize(static_cast<usize>(size)));
    if (ok && size != 0) {
        ok = static_cast<bool>(file.ReadExact(out.Data(), static_cast<usize>(size)).status);
    }

    const bool closed = static_cast<bool>(file.Close());
    return ok && closed ? Result<void>::Success() : Result<void>::Failure(ErrorCode::DependencyFailed);
}

} // namespace rocks::ui
