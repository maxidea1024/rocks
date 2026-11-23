#include "ToolFiles.h"
#include <Core/Containers/Array.h>
#include <Core/Guid.h>
#include <Core/Text/String.h>

namespace rocks::sparks::tools::detail {

namespace {

// 16진 글자 하나가 나타내는 bit 수다.
constexpr uint32 NibbleBits = 4;
constexpr uint32 NibbleMask = 0xF;
constexpr uint32 WordBits = 64;
constexpr StringView HexDigits = "0123456789abcdef";
constexpr StringView TemporarySuffix = ".tmp";

Result<void> AppendHexWord(StringBuilderBase& out, uint64 word) noexcept {
    for (uint32 shift = WordBits; shift > 0; shift -= NibbleBits) {
        const char digit = HexDigits.Data()[(word >> (shift - NibbleBits)) & NibbleMask];
        if (auto appended = out.TryAppend(StringView(&digit, 1)); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> AppendNewGuidDigits(StringBuilderBase& out) noexcept {
    auto guid = TryNewGuid();
    if (!guid) {
        return Result<void>::Failure(guid.Error());
    }

    if (auto high = AppendHexWord(out, guid.Value().high); !high) {
        return high;
    }

    return AppendHexWord(out, guid.Value().low);
}

Result<FileComparison> CompareFile(Allocator& allocator, platform::FileSystem& files,
                                   StringView path, Span<const uint8> bytes) noexcept {
    FileComparison result;
    result.status = files.FileExists(path.Data(), path.ByteCount(), result.exists);
    if (!result.status || !result.exists) {
        return Result<FileComparison>::Success(result);
    }

    platform::File file;
    result.status =
        file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
    if (!result.status) {
        return Result<FileComparison>::Success(result);
    }

    uint64 size = 0;
    result.status = file.Size(size);
    ErrorCode error = ErrorCode::None;
    if (result.status && size == bytes.Size()) {
        Array<uint8> content(allocator);
        if (auto resized = content.TryResize(bytes.Size()); !resized) {
            error = resized.Error();
        } else {
            const platform::FileTransfer read = file.ReadExact(content.Data(), content.Size());
            result.status = read.status;
            result.same = read.status && read.bytes == content.Size();
            for (usize at = 0; result.same && at < content.Size(); ++at) {
                result.same = content[at] == bytes[at];
            }
        }
    }

    const platform::FileStatus closed = file.Close();
    if (error != ErrorCode::None) {
        return Result<FileComparison>::Failure(error);
    }

    if (result.status && !closed) {
        result.status = closed;
    }

    return Result<FileComparison>::Success(result);
}

Result<platform::FileStatus> ReplaceFile(Allocator& allocator, platform::FileSystem& files,
                                         StringView path, Span<const uint8> bytes) noexcept {
    String temporary(allocator);
    {
        StringBuilder<GuidDigits> digits(allocator);
        if (auto made = AppendNewGuidDigits(digits); !made) {
            return Result<platform::FileStatus>::Failure(made.Error());
        }

        if (!temporary.TryAppend(path) || !temporary.TryAppend(".") ||
            !temporary.TryAppend(digits.View()) || !temporary.TryAppend(TemporarySuffix)) {
            return Result<platform::FileStatus>::Failure(ErrorCode::OutOfMemory);
        }
    }

    platform::File file;
    platform::FileStatus status = file.Open(files, temporary.Data(), temporary.ByteCount(),
                                            platform::FileOpenMode::CreateNew);
    if (!status) {
        return Result<platform::FileStatus>::Success(status);
    }

    const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
    status = written.status;
    if (status) {
        status = file.Flush();
    }

    const platform::FileStatus closed = file.Close();
    if (status && !closed) {
        status = closed;
    }

    if (status) {
        status =
            files.Replace(temporary.Data(), temporary.ByteCount(), path.Data(), path.ByteCount());
        if (status.code == platform::FileCode::NotDurable) {
            return Result<platform::FileStatus>::Success(status); // 교체는 이미 보인다
        }
    }

    if (!status) {
        static_cast<void>(files.Delete(temporary.Data(), temporary.ByteCount()));
    }

    return Result<platform::FileStatus>::Success(status);
}

} // namespace rocks::sparks::tools::detail
