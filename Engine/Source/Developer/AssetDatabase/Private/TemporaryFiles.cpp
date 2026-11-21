#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/TemporaryFiles.h>
#include <Core/FileError.h>

// 고아 임시 파일 정리다(Docs/Design/AssetSystem.ko.md 7.1절 12·16).
namespace rocks {

namespace {

using asset_database_detail::EditorIsOrphanTempName;
using asset_database_detail::EditorTempStem;

// 프로젝트 루트 상대 경로의 inline byte 수다.
constexpr usize TemporaryPathBytes = 512;
constexpr StringView ContentPrefix = "Content";
constexpr StringView IntermediatePrefix = "Intermediate";

// 열거한 항목 하나다(유효한 UTF-8 이름만 — 임시 파일 이름은 ASCII다).
struct TemporaryChild {
    explicit TemporaryChild(Allocator& allocator) noexcept : name(allocator) {
    }

    TemporaryChild(TemporaryChild&&) noexcept = default;
    TemporaryChild& operator=(TemporaryChild&&) noexcept = default;

    String name;
    platform::FileType type = platform::FileType::File;
};

struct TemporaryCollect {
    Array<TemporaryChild>* children;
    ErrorCode error = ErrorCode::None;
};

bool TemporaryCollectChild(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& context = *static_cast<TemporaryCollect*>(raw);
    const StringView name(entry.name, entry.nameBytes);
    if (entry.info.isLink || !name.IsValidUtf8() ||
        (entry.info.type != platform::FileType::File &&
         entry.info.type != platform::FileType::Directory)) {
        return true;
    }

    TemporaryChild child(context.children->GetAllocator());
    if (auto assigned = child.name.TryAssign(name); !assigned) {
        context.error = assigned.Error();
        return false;
    }

    child.type = entry.info.type;
    if (auto pushed = context.children->TryPushBack(Move(child)); !pushed) {
        context.error = pushed.Error();
        return false;
    }

    return true;
}

Result<void> AddTemporaryFailure(Array<TemporaryFileFailure>& failures, StringView prefix,
                                 StringView relative, ErrorCode error) noexcept {
    TemporaryFileFailure failure(failures.GetAllocator());
    const StringView pieces[] = {prefix, relative.IsEmpty() ? "" : "/", relative};
    for (StringView piece : pieces) {
        if (auto appended = failure.path.TryAppend(piece); !appended) {
            return appended;
        }
    }

    failure.error = error;
    return failures.TryPushBack(Move(failure));
}

// root(절대 경로) 아래를 recursive면 하위 디렉터리까지 걸어 matches 이름의 파일을 지운다.
template <class Matches>
Result<void> DeleteTemporaryUnder(platform::FileSystem& files, StringView root, StringView prefix,
                                  bool recursive, Matches&& matches, usize& removed,
                                  Array<TemporaryFileFailure>& failures) noexcept {
    Allocator& allocator = failures.GetAllocator();
    Array<String> pending(allocator);
    if (auto pushed = pending.TryPushBack(String(allocator)); !pushed) {
        return pushed;
    }

    while (!pending.IsEmpty()) {
        String relative = Move(pending[pending.Size() - 1]);
        (void)pending.TryPopBack();
        StringBuilder<TemporaryPathBytes> directory(allocator);
        const StringView pieces[] = {root, relative.IsEmpty() ? "" : "/", relative.View()};
        if (auto built = asset_database_detail::AppendAll(directory, pieces); !built) {
            return built;
        }

        Array<TemporaryChild> children(allocator);
        TemporaryCollect context{&children};
        if (auto listed =
                files.EnumerateDirectory(directory.View().Data(), directory.View().ByteCount(),
                                         &TemporaryCollectChild, &context);
            !listed) {
            if (listed.code == platform::FileCode::NotFound) {
                continue;
            }

            if (relative.IsEmpty()) {
                return Result<void>::Failure(FileErrorCode(listed));
            }

            if (auto added =
                    AddTemporaryFailure(failures, prefix, relative.View(), FileErrorCode(listed));
                !added) {
                return added;
            }

            continue;
        }

        if (context.error != ErrorCode::None) {
            return Result<void>::Failure(context.error);
        }

        for (TemporaryChild& child : children) {
            String childRelative(allocator);
            const StringView childPieces[] = {relative.View(), relative.IsEmpty() ? "" : "/",
                                              child.name.View()};
            for (StringView piece : childPieces) {
                if (auto appended = childRelative.TryAppend(piece); !appended) {
                    return appended;
                }
            }

            if (child.type == platform::FileType::Directory) {
                if (recursive) {
                    if (auto pushed = pending.TryPushBack(Move(childRelative)); !pushed) {
                        return pushed;
                    }
                }

                continue;
            }

            if (!matches(child.name.View())) {
                continue;
            }

            StringBuilder<TemporaryPathBytes> path(allocator);
            const StringView pathPieces[] = {root, "/", childRelative.View()};
            if (auto built = asset_database_detail::AppendAll(path, pathPieces); !built) {
                return built;
            }

            if (auto deleted = files.Delete(path.View().Data(), path.View().ByteCount());
                !deleted && deleted.code != platform::FileCode::NotFound) {
                if (auto added = AddTemporaryFailure(failures, prefix, childRelative.View(),
                                                     FileErrorCode(deleted));
                    !added) {
                    return added;
                }

                continue;
            }

            ++removed;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> DeleteTemporaryOrphans(platform::FileSystem& files, const ProjectLayout& layout,
                                    const AssetDatabaseLock& lock, usize& removed,
                                    Array<TemporaryFileFailure>& failures) noexcept {
    if (!lock.IsHeldFor(layout)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto content = DeleteTemporaryUnder(files, layout.ContentDirectory(), ContentPrefix, true,
                                            &EditorIsOrphanTempName, removed, failures);
        !content) {
        return content;
    }

    return DeleteTemporaryUnder(
        files, layout.IntermediateDirectory(), IntermediatePrefix, false,
        [](StringView name) noexcept { return EditorTempStem(name) == AssetIndexCacheFileName; },
        removed, failures);
}

} // namespace rocks
