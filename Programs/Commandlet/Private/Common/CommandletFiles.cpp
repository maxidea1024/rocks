#include "CommandletFiles.h"

#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>

namespace rocks::commandlet_host {

namespace {

// 파일 오류 한 줄의 버퍼 byte 수다(경로는 인수 상한까지 들어간다).
constexpr usize FileMessageBytes = CommandletRegistry::MaxArgumentBytes + 256;

} // namespace

PathText::PathText(StringView path) noexcept {
    const usize bytes = path.ByteCount() < sizeof(text) - 1 ? path.ByteCount() : sizeof(text) - 1;
    platform::MemoryCopy(text, path.Data(), bytes);
    text[bytes] = '\0';
}

Result<int32> ReportFileFailure(CommandletOutput& output, StringView command, const char* action,
                                StringView path, platform::FileStatus status) noexcept {
    char storage[FileMessageBytes];
    FixedStringBuilder text{Span<char>(storage)};
    (void)text.TryAppendFormat("{}: cannot {} '{}' (file code {}, native error {})\n", command,
                               action, path, static_cast<int32>(status.code), status.nativeError);
    auto written = output.write(output.context, CommandletStream::Error, text.View());
    return written ? Result<int32>::Success(1) : Result<int32>::Failure(written.Error());
}

bool ReadCommandFile(CommandletOutput& output, StringView command, StringView path,
                     Array<uint8>& bytes, Result<int32>& failure) noexcept {
    const PathText native(path);
    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(native.text, platform::FileOpenMode::ReadExisting);
        !opened) {
        failure = ReportFileFailure(output, command, "open", path, opened);
        return false;
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = file.Size(size); !sized) {
        failure = ReportFileFailure(output, command, "size", path, sized);
        return false;
    }

    if (!bytes.TryResize(static_cast<usize>(size))) {
        failure = Result<int32>::Failure(ErrorCode::OutOfMemory);
        return false;
    }

    if (const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
        !read.status) {
        failure = ReportFileFailure(output, command, "read", path, read.status);
        return false;
    }

    return true;
}

bool CreateCommandFile(CommandletOutput& output, StringView command, const PathText& path,
                       StringView shown, platform::File& file, Result<int32>& failure) noexcept {
    if (const platform::FileStatus opened = file.Open(path.text, platform::FileOpenMode::CreateNew);
        !opened) {
        failure = ReportFileFailure(output, command, "create", shown, opened);
        return false;
    }

    return true;
}

bool WriteCommandFileAndClose(CommandletOutput& output, StringView command, StringView shown,
                              platform::File& file, Span<const uint8> bytes,
                              Result<int32>& failure) noexcept {
    if (const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        !written.status) {
        failure = ReportFileFailure(output, command, "write", shown, written.status);
        return false;
    }

    if (const platform::FileStatus closed = file.Close(); !closed) {
        failure = ReportFileFailure(output, command, "close", shown, closed);
        return false;
    }

    return true;
}

Result<int32> WriteNewCommandFile(CommandletOutput& output, StringView command, StringView path,
                                  Span<const uint8> bytes) noexcept {
    const PathText native(path);
    platform::File file;
    Result<int32> failure = Result<int32>::Success(0);
    if (!CreateCommandFile(output, command, native, path, file, failure) ||
        !WriteCommandFileAndClose(output, command, path, file, bytes, failure)) {
        return failure;
    }

    return Result<int32>::Success(0);
}

} // namespace rocks::commandlet_host
