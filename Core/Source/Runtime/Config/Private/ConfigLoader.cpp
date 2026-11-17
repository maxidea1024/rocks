#include "ConfigState.h"

#include <Base/NumericLimits.h>
#include <Config/ConfigLoader.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

namespace rocks::config {

using detail::ConfigState;

namespace {

ErrorCode ToErrorCode(platform::FileCode code) noexcept {
    switch (code) {
    case platform::FileCode::NotFound:
        return ErrorCode::NotFound;
    case platform::FileCode::OutOfMemory:
        return ErrorCode::OutOfMemory;
    case platform::FileCode::Busy:
        return ErrorCode::Busy;
    case platform::FileCode::Unsupported:
        return ErrorCode::Unsupported;
    case platform::FileCode::Overflow:
        return ErrorCode::Overflow;
    default:
        return ErrorCode::InvalidArgument;
    }
}

// path 파일을 out에 통째로 읽는다. 반환: 없으면 false(out은 비운다).
Result<bool> ReadWholeFile(platform::FileSystem& files, StringView path,
                           Array<char>& out) noexcept {
    out.Clear();
    bool exists = false;
    if (auto checked = files.Exists(path.Data(), path.ByteCount(), exists); !checked) {
        return Result<bool>::Failure(ToErrorCode(checked.code));
    }

    if (!exists) {
        return Result<bool>::Success(false);
    }

    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        return Result<bool>::Failure(ToErrorCode(opened.code));
    }

    uint64 size = 0;
    platform::FileStatus status = file.Size(size);
    if (status && size > static_cast<uint64>(NumericLimits<usize>::Max())) {
        status.code = platform::FileCode::Overflow;
    }

    Result<void> resized = Result<void>::Success();
    if (status) {
        resized = out.TryResize(static_cast<usize>(size));
    }

    if (status && resized && size != 0) {
        status = file.ReadExact(out.Data(), static_cast<usize>(size)).status;
    }

    const platform::FileStatus closed = file.Close();
    if (!resized) {
        return Result<bool>::Failure(resized.Error());
    }

    if (!status || !closed) {
        return Result<bool>::Failure(ToErrorCode(!status ? status.code : closed.code));
    }

    return Result<bool>::Success(true);
}

// 파일 하나를 층으로 넣는다. 반환: 진단으로 거부됐으면 false.
Result<bool> LoadOne(ConfigContext& context, platform::FileSystem& files, ConfigLayer layer,
                     StringView root, StringView folder, StringView branch,
                     Array<char>& buffer) noexcept {
    ConfigState& state = *context.State();
    StringBuilder<256> tag(state.allocator);
    if (!folder.IsEmpty()) {
        static_cast<void>(tag.TryAppend(folder));
        static_cast<void>(tag.TryAppend("/"));
    }

    static_cast<void>(tag.TryAppend(branch));
    static_cast<void>(tag.TryAppend(".json"));
    StringBuilder<256> path(state.allocator);
    static_cast<void>(path.TryAppend(root));
    static_cast<void>(path.TryAppend("/Config/"));
    static_cast<void>(path.TryAppend(tag.View()));
    if (auto status = tag.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    if (auto status = path.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    auto read = ReadWholeFile(files, path.View(), buffer);
    if (!read) {
        return read;
    }

    // 없는 파일은 건너뛴다(거부가 아니다).
    if (!read.Value()) {
        return Result<bool>::Success(true);
    }

    auto added = context.AddLayerText(layer, tag.View(), branch, path.View(),
                                      StringView(buffer.Data(), buffer.Size()));
    if (!added) {
        if (added.Error() == ErrorCode::InvalidArgument) {
            return Result<bool>::Success(false);
        }

        return Result<bool>::Failure(added.Error());
    }

    return Result<bool>::Success(true);
}

} // namespace

Result<void> LoadConfigFiles(ConfigContext& context, platform::FileSystem& files) noexcept {
    ConfigState* state = context.State();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    // 플랫폼 폴더(약한 것부터): 부모 사슬 → 플랫폼.
    Array<StringView> platforms(state->allocator);
    for (const String& parent : state->platformParents) {
        if (auto pushed = platforms.TryPushBack(parent.View()); !pushed) {
            return pushed;
        }
    }

    if (!state->platform.IsEmpty()) {
        if (auto pushed = platforms.TryPushBack(state->platform.View()); !pushed) {
            return pushed;
        }
    }

    StringBuilder<256> user(state->allocator);
    const bool readUser =
        state->mode == ConfigMode::Standard &&
        !state->directories[static_cast<usize>(ConfigPathToken::UserDataDir)].IsEmpty() &&
        !state->projectName.IsEmpty();
    if (readUser) {
        static_cast<void>(user.TryAppend(
            state->directories[static_cast<usize>(ConfigPathToken::UserDataDir)].View()));
        static_cast<void>(user.TryAppend("/"));
        static_cast<void>(user.TryAppend(state->projectName.View()));
        if (auto status = user.Status(); !status) {
            return status;
        }
    }

    const StringView engine =
        state->directories[static_cast<usize>(ConfigPathToken::EngineDir)].View();
    const StringView project =
        state->directories[static_cast<usize>(ConfigPathToken::ProjectDir)].View();
    Array<char> buffer(state->allocator);
    bool rejected = false;
    // branch 목록은 AddLayerText 도중 바뀌지 않는다(등록은 이 함수 밖에서만 한다).
    for (usize branchIndex = 0; branchIndex < state->branches.Size(); ++branchIndex) {
        const StringView branch = state->branches[branchIndex].View();
        struct Source {
            ConfigLayer layer;
            StringView root;
            StringView folder;
        };

        Array<Source> sources(state->allocator);
        Result<void> planned = Result<void>::Success();
        if (!engine.IsEmpty()) {
            planned = sources.TryPushBack(Source{ConfigLayer::Engine, engine, {}});
            for (usize index = 0; planned && index < platforms.Size(); ++index) {
                planned = sources.TryPushBack(
                    Source{ConfigLayer::EnginePlatform, engine, platforms[index]});
            }
        }

        if (planned && !project.IsEmpty()) {
            planned = sources.TryPushBack(Source{ConfigLayer::Project, project, {}});
            for (usize index = 0; planned && index < platforms.Size(); ++index) {
                planned = sources.TryPushBack(
                    Source{ConfigLayer::ProjectPlatform, project, platforms[index]});
            }
        }

        if (planned && readUser) {
            planned = sources.TryPushBack(Source{ConfigLayer::User, user.View(), {}});
        }

        if (!planned) {
            return planned;
        }

        for (const Source& source : sources) {
            auto loaded =
                LoadOne(context, files, source.layer, source.root, source.folder, branch, buffer);
            if (!loaded) {
                return Result<void>::Failure(loaded.Error());
            }

            rejected = rejected || !loaded.Value();
        }
    }

    return rejected ? Result<void>::Failure(ErrorCode::InvalidArgument) : Result<void>::Success();
}

Result<uint32> AddCommandLineAssignments(ConfigContext& context,
                                         Span<const StringView> arguments) noexcept {
    uint32 added = 0;
    ErrorCode first = ErrorCode::None;
    for (const StringView argument : arguments) {
        if (!argument.StartsWith(SetArgumentPrefix, StringComparison::CaseSensitive)) {
            continue;
        }

        const StringView assignment =
            argument.TryRightBytes(argument.ByteCount() - SetArgumentPrefix.ByteCount()).Value();
        StringView target;
        StringView value;
        ErrorCode error = ErrorCode::None;
        if (!assignment.Split("=", target, value, StringComparison::CaseSensitive) ||
            target.IsEmpty()) {
            error = ErrorCode::InvalidArgument;
        } else if (auto queued =
                       context.AddAssignment(ConfigLayer::Session, "--set", target, value);
                   !queued) {
            error = queued.Error();
        } else {
            ++added;
        }

        if (error != ErrorCode::None && first == ErrorCode::None) {
            first = error;
        }
    }

    return first == ErrorCode::None ? Result<uint32>::Success(added)
                                    : Result<uint32>::Failure(first);
}

} // namespace rocks::config
