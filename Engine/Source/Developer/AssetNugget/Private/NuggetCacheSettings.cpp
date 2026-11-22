#include <AssetNugget/NuggetCacheSettings.h>
#include <Core/Containers/Array.h>
#include <Core/FileError.h>
#include <Core/Text/Json.h>
#include <Core/Text/Path.h>

namespace rocks {

namespace {

constexpr StringView SettingsBackendKey = "backend";
constexpr StringView SettingsRequiredKey = "required";
constexpr StringView SettingsLocalPathKey = "localPath";
constexpr StringView SettingsGenerationKey = "generation";
// Guid 표준 문자열 길이다(36자 — 하이픈 형식).
constexpr usize SettingsGuidTextBytes = 36;
// 설정 JSON의 노드 상한이다(루트 하나 + 칸 넷 — 넉넉히).
constexpr uint32 SettingsMaxNodes = 64;

struct BackendName {
    StringView text;
    NuggetBackendKind kind;
};

constexpr BackendName BackendNames[] = {{"filesystem", NuggetBackendKind::FileSystem},
                                        {"rockpile", NuggetBackendKind::Rockpile},
                                        {"off", NuggetBackendKind::Off},
                                        {"auto", NuggetBackendKind::Auto}};

Result<NuggetCacheSettings> Reject(NuggetSettingsFailure* failure, NuggetSettingsField field,
                                   ErrorCode error = ErrorCode::InvalidArgument) noexcept {
    if (failure != nullptr) {
        failure->field = field;
    }

    return Result<NuggetCacheSettings>::Failure(error);
}

// path 파일 전체를 읽는다. 없으면 existed = false로 성공한다.
Result<void> ReadSettingsFile(platform::FileSystem& files, StringView path, Array<uint8>& bytes,
                              bool& existed, bool& tooLarge) noexcept {
    existed = false;
    tooLarge = false;
    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        if (opened.code == platform::FileCode::NotFound) {
            return Result<void>::Success();
        }

        return Result<void>::Failure(FileErrorCode(opened));
    }

    existed = true;
    uint64 size = 0;
    if (auto sized = file.Size(size); !sized) {
        return Result<void>::Failure(FileErrorCode(sized));
    }

    if (size > NuggetSettingsMaxBytes) {
        tooLarge = true;
        return Result<void>::Success();
    }

    if (auto resized = bytes.TryResize(static_cast<usize>(size)); !resized) {
        return resized;
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    return read.status ? Result<void>::Success()
                       : Result<void>::Failure(FileErrorCode(read.status));
}

// localPath로 쓸 수 있는 경로인지다: 상대·POSIX 루트·Windows 드라이브 절대·UNC.
bool IsUsableLocalPath(StringView path) noexcept {
    auto root = Path::Root(path);
    return root && root.Value() != PathRoot::WindowsCurrentDriveRoot &&
           root.Value() != PathRoot::WindowsDriveRelative;
}

} // namespace

Result<NuggetCacheSettings> ReadNuggetCacheSettings(Allocator& allocator,
                                                    platform::FileSystem& files,
                                                    StringView projectRoot,
                                                    NuggetSettingsFailure* failure) noexcept {
    if (failure != nullptr) {
        *failure = NuggetSettingsFailure{};
    }

    NuggetCacheSettings settings(allocator);
    if (auto assigned = settings.localPath.TryAssign(NuggetDefaultLocalPath); !assigned) {
        return Result<NuggetCacheSettings>::Failure(assigned.Error());
    }

    String path(allocator);
    Result<void> step = path.TryAppend(projectRoot);
    if (step) {
        step = path.TryAppend("/");
    }

    if (step) {
        step = path.TryAppend(NuggetSettingsFile);
    }

    if (!step) {
        return Result<NuggetCacheSettings>::Failure(step.Error());
    }

    Array<uint8> bytes(allocator);
    bool existed = false;
    bool tooLarge = false;
    if (auto read = ReadSettingsFile(files, path.View(), bytes, existed, tooLarge); !read) {
        return Result<NuggetCacheSettings>::Failure(read.Error());
    }

    if (tooLarge) {
        return Reject(failure, NuggetSettingsField::File, ErrorCode::CapacityExceeded);
    }

    if (!existed) {
        return Result<NuggetCacheSettings>::Success(Move(settings));
    }

    JsonDocument document(allocator);
    JsonReadOptions options;
    options.maxNodes = SettingsMaxNodes;
    const JsonError error = document.Parse(
        StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()), options);
    if (!error.Succeeded()) {
        if (error.code == JsonErrorCode::OutOfMemory) {
            return Result<NuggetCacheSettings>::Failure(ErrorCode::OutOfMemory);
        }

        if (failure != nullptr) {
            failure->line = error.position.line;
            failure->column = error.position.column;
        }

        return Reject(failure, NuggetSettingsField::Json);
    }

    const JsonValue root = document.Root();
    if (!root.IsObject()) {
        return Reject(failure, NuggetSettingsField::Root);
    }

    for (usize index = 0; index < root.Size(); ++index) {
        const StringView key = root.KeyAt(index);
        const JsonValue value = root.ValueAt(index);
        if (key == SettingsBackendKey) {
            const BackendName* found = nullptr;
            for (const BackendName& name : BackendNames) {
                found = value.IsString() && value.GetString() == name.text ? &name : found;
            }

            if (found == nullptr) {
                return Reject(failure, NuggetSettingsField::Backend);
            }

            settings.backend = found->kind;
        } else if (key == SettingsRequiredKey) {
            if (!value.IsBool()) {
                return Reject(failure, NuggetSettingsField::Required);
            }

            settings.required = value.GetBool();
        } else if (key == SettingsLocalPathKey) {
            if (!value.IsString() || value.GetString().IsEmpty() ||
                value.GetString().Contains(StringView("\0", 1), StringComparison::CaseSensitive)) {
                return Reject(failure, NuggetSettingsField::LocalPath);
            }

            // 프로세스의 현재 드라이브·디렉터리에 따라 위치가 바뀌는 Windows 경로(현재 드라이브
            // 루트
            // `\x`·드라이브 상대 `C:x`)와 device namespace는 거부한다(A2f1a 원본 루트와 같다).
            if (!IsUsableLocalPath(value.GetString())) {
                return Reject(failure, NuggetSettingsField::LocalPath);
            }

            if (auto assigned = settings.localPath.TryAssign(value.GetString()); !assigned) {
                return Result<NuggetCacheSettings>::Failure(assigned.Error());
            }
        } else if (key == SettingsGenerationKey) {
            // 표준 문자열(36자 — 하이픈 형식)만 받는다.
            auto parsed = value.IsString() && value.GetString().ByteCount() == SettingsGuidTextBytes
                              ? TryParseGuid(value.GetString())
                              : Result<Guid>::Failure(ErrorCode::InvalidArgument);
            if (!parsed) {
                return Reject(failure, NuggetSettingsField::Generation);
            }

            settings.generation = parsed.Value();
        } else {
            return Reject(failure, NuggetSettingsField::UnknownKey);
        }
    }

    return Result<NuggetCacheSettings>::Success(Move(settings));
}

Result<void> ResolveNuggetLocalPath(const NuggetCacheSettings& settings, StringView projectRoot,
                                    String& out) noexcept {
    out.Clear();
    const StringView local = settings.localPath.View();
    auto root = Path::Root(local);
    if (!root) {
        return Result<void>::Failure(root.Error());
    }

    if (!IsUsableLocalPath(local)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (root.Value() != PathRoot::Relative) {
        return out.TryAssign(local);
    }

    Result<void> step = out.TryAppend(projectRoot);
    if (step) {
        step = out.TryAppend("/");
    }

    if (step) {
        step = out.TryAppend(local);
    }

    return step;
}

} // namespace rocks
