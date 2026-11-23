#include <AssetDatabase/AtomicFileWrite.h>

#include "AssetDatabaseInternal.h"

#include <Core/FileError.h>
#include <Core/Guid.h>
#include <Core/Text/String.h>

// 작은 파일의 원자 교체 쓰기다(Docs/Design/AssetSystem.ko.md 7.1절 16·17·10.2절 A4d).
namespace rocks {

using asset_database_detail::EditorAppendGuidText;
using asset_database_detail::EditorTempSuffix;

Result<AtomicWriteOutcome> WriteFileAtomically(platform::FileSystem& files, Allocator& allocator,
                                               StringView directory, StringView fileName,
                                               Span<const uint8> bytes) noexcept {
    using Out = AtomicWriteOutcome;
    if (auto made = platform::MakeDirectoryTree(files, directory.Data(), directory.ByteCount());
        !made) {
        return Result<Out>::Failure(FileErrorCode(made));
    }

    auto guid = TryNewGuid();
    if (!guid) {
        return Result<Out>::Failure(guid.Error());
    }

    String target(allocator);
    String temp(allocator);
    const StringView targetPieces[] = {directory, "/", fileName};
    Result<void> step = Result<void>::Success();
    for (StringView piece : targetPieces) {
        if (step) {
            step = target.TryAppend(piece);
        }
    }

    if (step) {
        step = temp.TryAssign(target.View());
    }

    if (step) {
        step = temp.TryAppend(".");
    }

    if (step) {
        step = EditorAppendGuidText(temp, guid.Value());
    }

    if (step) {
        step = temp.TryAppend(EditorTempSuffix);
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    auto discard = [&]() noexcept {
        (void)files.Delete(temp.View().Data(), temp.View().ByteCount());
    };
    {
        platform::File file;
        if (auto opened = file.Open(files, temp.View().Data(), temp.View().ByteCount(),
                                    platform::FileOpenMode::CreateNew);
            !opened) {
            return Result<Out>::Failure(FileErrorCode(opened));
        }

        const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        if (!written.status) {
            (void)file.Close();
            discard();
            return Result<Out>::Failure(FileErrorCode(written.status));
        }

        if (auto flushed = file.Flush(); !flushed) {
            (void)file.Close();
            discard();
            return Result<Out>::Failure(FileErrorCode(flushed));
        }

        if (auto closed = file.Close(); !closed) {
            discard();
            return Result<Out>::Failure(FileErrorCode(closed));
        }
    }

    const platform::FileStatus replaced =
        files.Replace(temp.View().Data(), temp.View().ByteCount(), target.View().Data(),
                      target.View().ByteCount());
    if (!replaced && replaced.code != platform::FileCode::NotDurable) {
        discard();
        return Result<Out>::Failure(FileErrorCode(replaced));
    }

    Out outcome;
    outcome.notDurable = !replaced;
    return Result<Out>::Success(outcome);
}

} // namespace rocks
