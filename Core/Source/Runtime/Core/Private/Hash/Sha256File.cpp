#include <Core/Hash/Sha256File.h>
#include <Core/FileError.h>

namespace rocks {

Result<void> HashFileSha256(platform::File& file, Span<uint8> buffer, Sha256Digest& digest,
                            uint64& bytes) noexcept {
    if (buffer.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Sha256 hasher;
    bytes = 0;
    for (;;) {
        const platform::FileTransfer read = file.Read(buffer.Data(), buffer.Size());
        if (!read.status) {
            return Result<void>::Failure(FileErrorCode(read.status));
        }

        if (read.bytes == 0) {
            break;
        }

        hasher.Update(Span<const uint8>(buffer.Data(), read.bytes));
        bytes += read.bytes;
    }

    digest = hasher.Finish();
    return Result<void>::Success();
}

} // namespace rocks
