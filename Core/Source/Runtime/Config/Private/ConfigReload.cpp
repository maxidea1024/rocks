#include "ConfigState.h"

#include <Config/ConfigReload.h>
#include <Config/UserConfig.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

namespace rocks::config {

using detail::ConfigState;

namespace {

constexpr usize RootCount = 3;

// 다시 읽을 표시 하나다(relative가 비면 루트 전체).
struct ReloadMark {
    explicit ReloadMark(Allocator& allocator) noexcept : relative(allocator) {
    }

    ConfigFileRoot root = ConfigFileRoot::Engine;
    String relative;
};

ConfigLayer BaseLayer(ConfigFileRoot root) noexcept {
    switch (root) {
    case ConfigFileRoot::Engine:
        return ConfigLayer::Engine;
    case ConfigFileRoot::Project:
        return ConfigLayer::Project;
    case ConfigFileRoot::User:
        return ConfigLayer::User;
    }

    return ConfigLayer::Engine;
}

ConfigLayer PlatformLayer(ConfigFileRoot root) noexcept {
    return root == ConfigFileRoot::Engine ? ConfigLayer::EnginePlatform
                                          : ConfigLayer::ProjectPlatform;
}

} // namespace

struct ConfigFileReloader::State {
    explicit State(Allocator& allocator) noexcept
        : marks(allocator), roots{String(allocator), String(allocator), String(allocator)} {
    }

    bool started = false;
    Array<UniquePtr<ReloadMark>> marks;
    // ConfigFileRoot 순서의 Config 폴더 경로(비면 그 루트가 없다).
    String roots[RootCount];
    platform::DirectoryWatcher watchers[RootCount];
};

ConfigFileReloader::ConfigFileReloader(ConfigContext& context, platform::FileSystem& files) noexcept
    : context_(context), files_(files) {
}

ConfigFileReloader::~ConfigFileReloader() noexcept {
    Stop();
}

Result<void> ConfigFileReloader::Start(bool watchDirectories) noexcept {
    ConfigState* config = context_.State();
    if (config == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != config->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (state_.Get() != nullptr && state_->started) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    // 출시 빌드는 감시하지 않는다(8절).
    if (config->releaseRules) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto made = UniquePtr<State>::TryMake(config->allocator, config->allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    State& state = *made.Value();
    const StringView engine =
        config->directories[static_cast<usize>(ConfigPathToken::EngineDir)].View();
    const StringView project =
        config->directories[static_cast<usize>(ConfigPathToken::ProjectDir)].View();
    Result<void> filled = Result<void>::Success();
    if (!engine.IsEmpty()) {
        filled = state.roots[static_cast<usize>(ConfigFileRoot::Engine)].TryAssign(engine);
        if (filled) {
            filled = state.roots[static_cast<usize>(ConfigFileRoot::Engine)].TryAppend("/Config");
        }
    }

    if (filled && !project.IsEmpty()) {
        filled = state.roots[static_cast<usize>(ConfigFileRoot::Project)].TryAssign(project);
        if (filled) {
            filled = state.roots[static_cast<usize>(ConfigFileRoot::Project)].TryAppend("/Config");
        }
    }

    if (filled) {
        // 봉인 모드·사용자 폴더 없음은 사용자 루트가 없을 뿐이다.
        const Result<void> user =
            UserConfigDirectory(context_, state.roots[static_cast<usize>(ConfigFileRoot::User)]);
        if (!user && user.Error() != ErrorCode::Unsupported) {
            filled = Result<void>::Failure(user.Error());
        }
    }

    if (!filled) {
        return filled;
    }

    if (watchDirectories) {
        ErrorCode firstError = ErrorCode::None;
        bool opened = false;
        for (usize index = 0; index < RootCount; ++index) {
            const String& root = state.roots[index];
            if (root.IsEmpty()) {
                continue;
            }

            if (index == static_cast<usize>(ConfigFileRoot::User)) {
                static_cast<void>(
                    platform::MakeDirectoryTree(files_, root.Data(), root.ByteCount()));
            }

            const platform::FileStatus status =
                state.watchers[index].Open(root.Data(), root.ByteCount());
            if (status) {
                opened = true;
                continue;
            }

            // 없는 루트(프로젝트에 Config 폴더가 없다 등)는 감시하지 않는다.
            if (status.code != platform::FileCode::NotFound && firstError == ErrorCode::None) {
                firstError = FileErrorCode(status);
            }

            Log::Debug(detail::LogConfig, "Config folder {Path} is not watched (file code {Code})",
                       root.View(), static_cast<uint32>(status.code));
        }

        if (!opened && firstError != ErrorCode::None) {
            return Result<void>::Failure(firstError);
        }
    }

    if (auto hooked = context_.AddSyncHook(&ConfigFileReloader::OnSync, this); !hooked) {
        return hooked;
    }

    state.started = true;
    state_ = Move(made.Value());
    return Result<void>::Success();
}

void ConfigFileReloader::Stop() noexcept {
    if (state_.Get() == nullptr) {
        return;
    }

    if (state_->started) {
        static_cast<void>(context_.RemoveSyncHook(&ConfigFileReloader::OnSync, this));
    }

    for (platform::DirectoryWatcher& watcher : state_->watchers) {
        watcher.Close();
    }

    state_ = nullptr;
}

Result<void> ConfigFileReloader::NotifyChanged(ConfigFileRoot root, StringView relative) noexcept {
    if (static_cast<usize>(root) >= RootCount || state_.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const UniquePtr<ReloadMark>& mark : state_->marks) {
        if (mark->root == root && (mark->relative.IsEmpty() || mark->relative.View() == relative)) {
            return Result<void>::Success();
        }
    }

    ConfigState& config = *context_.State();
    auto made = UniquePtr<ReloadMark>::TryMake(config.allocator, config.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    made.Value()->root = root;
    if (auto assigned = made.Value()->relative.TryAssign(relative); !assigned) {
        return assigned;
    }

    return state_->marks.TryPushBack(Move(made.Value()));
}

bool ConfigFileReloader::WaitForChanges(ConfigFileRoot root, uint32 timeoutMilliseconds) noexcept {
    if (state_.Get() == nullptr || static_cast<usize>(root) >= RootCount) {
        return false;
    }

    return state_->watchers[static_cast<usize>(root)].Wait(timeoutMilliseconds);
}

void ConfigFileReloader::OnSync(void* self, ConfigContext& context) noexcept {
    auto& reloader = *static_cast<ConfigFileReloader*>(self);
    static_cast<void>(context);
    reloader.PollWatchers();
    auto reloaded = reloader.ReloadMarked();
    if (!reloaded) {
        Log::Error(detail::LogConfig, "Config hot reload failed: {Error}",
                   ErrorCodeName(reloaded.Error()));
        reloader.lastReloadCount_ = 0;
        return;
    }

    reloader.lastReloadCount_ = reloaded.Value();
}

void ConfigFileReloader::PollWatchers() noexcept {
    struct Visit {
        ConfigFileReloader* self;
        ConfigFileRoot root;
    };

    for (usize index = 0; index < RootCount; ++index) {
        platform::DirectoryWatcher& watcher = state_->watchers[index];
        if (!watcher.IsOpen()) {
            continue;
        }

        Visit visit{this, static_cast<ConfigFileRoot>(index)};
        const platform::FileStatus polled = watcher.Poll(
            [](const platform::DirectoryChange& change, void* raw) noexcept {
                auto& context = *static_cast<Visit*>(raw);
                StringView path(change.path, change.pathBytes);
                if (change.kind == platform::DirectoryChangeKind::Overflow ||
                    change.kind == platform::DirectoryChangeKind::RootRemoved) {
                    path = {};
                }

                // 이름 변경은 옛 이름(지움)과 새 이름(생김) 둘 다 다시 본다.
                if (change.kind == platform::DirectoryChangeKind::Renamed) {
                    static_cast<void>(context.self->NotifyChanged(
                        context.root, StringView(change.oldPath, change.oldPathBytes)));
                }

                static_cast<void>(context.self->NotifyChanged(context.root, path));
                return true;
            },
            &visit);
        if (!polled) {
            Log::Warning(detail::LogConfig,
                         "Config folder {Path} watch poll failed (file code {Code}); rereading it",
                         state_->roots[index].View(), static_cast<uint32>(polled.code));
            static_cast<void>(NotifyChanged(static_cast<ConfigFileRoot>(index), {}));
        }
    }
}

Result<uint32> ConfigFileReloader::ReloadMarked() noexcept {
    ConfigState& config = *context_.State();
    // 표시를 꺼내 비운다(다시 읽는 동안의 새 표시는 다음 sync 지점에 처리한다).
    Array<UniquePtr<ReloadMark>> marks = Move(state_->marks);
    state_->marks = Array<UniquePtr<ReloadMark>>(config.allocator);
    // 루트 전체 표시는 그 루트의 모든 branch 파일(플랫폼 폴더 포함) 표시로 펼친다.
    Array<UniquePtr<ReloadMark>> files(config.allocator);
    for (UniquePtr<ReloadMark>& mark : marks) {
        if (!mark->relative.IsEmpty()) {
            if (auto pushed = files.TryPushBack(Move(mark)); !pushed) {
                return Result<uint32>::Failure(pushed.Error());
            }

            continue;
        }

        Array<StringView> folders(config.allocator);
        if (auto pushed = folders.TryPushBack(StringView{}); !pushed) {
            return Result<uint32>::Failure(pushed.Error());
        }

        if (mark->root != ConfigFileRoot::User) {
            for (const String& parent : config.platformParents) {
                if (auto pushed = folders.TryPushBack(parent.View()); !pushed) {
                    return Result<uint32>::Failure(pushed.Error());
                }
            }

            if (!config.platform.IsEmpty()) {
                if (auto pushed = folders.TryPushBack(config.platform.View()); !pushed) {
                    return Result<uint32>::Failure(pushed.Error());
                }
            }
        }

        for (const String& branch : config.branches) {
            for (const StringView folder : folders) {
                auto made = UniquePtr<ReloadMark>::TryMake(config.allocator, config.allocator);
                if (!made) {
                    return Result<uint32>::Failure(made.Error());
                }

                made.Value()->root = mark->root;
                StringBuilder<128> relative(config.allocator);
                if (!folder.IsEmpty()) {
                    static_cast<void>(relative.TryAppendFormat("{}/", folder));
                }

                static_cast<void>(relative.TryAppendFormat("{}.json", branch.View()));
                Result<void> assigned = relative.Status();
                if (assigned) {
                    assigned = made.Value()->relative.TryAssign(relative.View());
                }

                if (assigned) {
                    assigned = files.TryPushBack(Move(made.Value()));
                }

                if (!assigned) {
                    return Result<uint32>::Failure(assigned.Error());
                }
            }
        }
    }

    uint32 reloaded = 0;
    Array<char> buffer(config.allocator);
    for (const UniquePtr<ReloadMark>& mark : files) {
        const String& root = state_->roots[static_cast<usize>(mark->root)];
        if (root.IsEmpty()) {
            continue;
        }

        // `<폴더>/<Branch>.json` 또는 `<Branch>.json`만 설정 파일이다.
        const StringView relative = mark->relative.View();
        StringView folder;
        StringView file = relative;
        if (!relative.Split("/", folder, file, StringComparison::CaseSensitive,
                            StringSearchDirection::FromEnd)) {
            folder = {};
            file = relative;
        }

        constexpr StringView Extension = ".json";
        if (!file.EndsWith(Extension, StringComparison::CaseSensitive)) {
            continue;
        }

        const StringView branch =
            file.TryLeftBytes(file.ByteCount() - Extension.ByteCount()).Value();
        if (config.FindBranch(branch) == IndexNone) {
            continue;
        }

        ConfigLayer layer = BaseLayer(mark->root);
        if (!folder.IsEmpty()) {
            bool known = mark->root != ConfigFileRoot::User && config.platform.View() == folder;
            for (const String& parent : config.platformParents) {
                known = known || (mark->root != ConfigFileRoot::User && parent.View() == folder);
            }

            if (!known) {
                continue;
            }

            layer = PlatformLayer(mark->root);
        }

        StringBuilder<256> path(config.allocator);
        static_cast<void>(path.TryAppendFormat("{}/{}", root.View(), relative));
        if (auto status = path.Status(); !status) {
            return Result<uint32>::Failure(status.Error());
        }

        auto read = detail::ReadWholeFile(files_, path.View(), buffer);
        if (!read) {
            Log::Warning(detail::LogConfig,
                         "Config file {Path} could not be read for reload: {Error}", path.View(),
                         ErrorCodeName(read.Error()));
            continue;
        }

        const ConfigDiagnosticsMark before = config.diagnostics.Mark();
        Result<void> queued = Result<void>::Success();
        if (!read.Value()) {
            queued = context_.Unset(layer, relative);
        } else if (layer == ConfigLayer::User) {
            queued = AddUserFileText(context_, relative, branch, path.View(),
                                     StringView(buffer.Data(), buffer.Size()), true);
        } else {
            queued = context_.ReplaceLayerText(layer, relative, branch, path.View(),
                                               StringView(buffer.Data(), buffer.Size()));
        }

        if (!queued) {
            if (queued.Error() != ErrorCode::InvalidArgument) {
                return Result<uint32>::Failure(queued.Error());
            }

            // 거부된 내용: 옛 값을 두고 진단을 남긴다.
            Log::Error(detail::LogConfig,
                       "Config file {Path} was not reloaded; the previous values stay",
                       path.View());
            for (usize index = before.count; index < config.diagnostics.Count(); ++index) {
                const ConfigDiagnostic item = config.diagnostics.Get(index);
                if (item.severity != ConfigSeverity::Error) {
                    continue;
                }

                Log::Error(detail::LogConfig,
                           "  {Source}:{Line}:{Column} {Code} at '{Path}': {Message}", item.source,
                           item.position.line, item.position.column,
                           ConfigDiagnosticCodeName(item.code), item.path, item.message);
            }

            continue;
        }

        if (read.Value()) {
            Log::Info(detail::LogConfig, "Config file {Path} reloaded", path.View());
        } else {
            Log::Info(detail::LogConfig, "Config file {Path} removed; its values are unset",
                      path.View());
        }

        ++reloaded;
    }

    return Result<uint32>::Success(reloaded);
}

} // namespace rocks::config
