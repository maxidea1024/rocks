#include <Core/Move.h>
#include <Platform/File.h>
#include <Platform/PlatformMath.h>
#include <SplashScreen/SplashImage.h>

namespace rocks::splash {

namespace {

// 설정 경로에서 펼치는 변수다(Config 경로 변수와 같은 이름).
constexpr StringView ProjectDirVariable = "{ProjectDir}";
constexpr StringView EngineDirVariable = "{EngineDir}";

// 폴더 아래 스플래시 자리다.
constexpr StringView SplashContentFolder = "Content/Splash/";

// 배율 하한 비교값이다(1.0 = 96 DPI — 그 이하면 1x를 쓴다).
constexpr double NativeScale = 1.0;

// 창 크기 계산이 받는 배율 상한이다(OS가 주는 최대 배율 500%보다 넉넉하다 — 크기 넘침 방지).
constexpr double MaxScale = 8.0;

bool StartsWithAt(StringView text, usize offset, StringView prefix) noexcept {
    if (offset + prefix.ByteCount() > text.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < prefix.ByteCount(); ++index) {
        if (text.Data()[offset + index] != prefix.Data()[index]) {
            return false;
        }
    }

    return true;
}

// {ProjectDir}·{EngineDir}를 펼친다. 다른 {…}는 InvalidArgument다.
Result<void> ExpandPath(String& out, StringView path, StringView projectDirectory,
                        StringView engineDirectory) noexcept {
    out.Clear();
    usize literalStart = 0;
    for (usize index = 0; index < path.ByteCount(); ++index) {
        if (path.Data()[index] != '{') {
            continue;
        }

        if (auto appended = out.TryAppend(path.SliceBytes(literalStart, index - literalStart));
            !appended) {
            return appended;
        }

        StringView replacement;
        usize skip = 0;
        if (StartsWithAt(path, index, ProjectDirVariable)) {
            replacement = projectDirectory;
            skip = ProjectDirVariable.ByteCount();
        } else if (StartsWithAt(path, index, EngineDirVariable)) {
            replacement = engineDirectory;
            skip = EngineDirVariable.ByteCount();
        } else {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto appended = out.TryAppend(replacement); !appended) {
            return appended;
        }

        index += skip - 1;
        literalStart = index + 1;
    }

    return out.TryAppend(path.SliceBytes(literalStart, path.ByteCount() - literalStart));
}

bool Exists(platform::FileSystem& files, const String& path) noexcept {
    bool exists = false;
    const platform::FileStatus status = files.FileExists(path.Data(), path.ByteCount(), exists);
    return static_cast<bool>(status) && exists;
}

// base(확장자 앞까지)에 꼬리와 확장자를 붙인 경로다.
Result<void> ComposePath(String& out, StringView base, StringView suffix,
                         StringView extension) noexcept {
    out.Clear();
    if (auto appended = out.TryAppend(base); !appended) {
        return appended;
    }

    if (auto appended = out.TryAppend(suffix); !appended) {
        return appended;
    }

    return out.TryAppend(extension);
}

// 경로의 마지막 마디에서 확장자('.'부터)가 시작하는 위치다. 없으면 길이다.
usize ExtensionOffset(StringView path) noexcept {
    for (usize index = path.ByteCount(); index > 0; --index) {
        const char character = path.Data()[index - 1];
        if (character == '/' || character == '\\') {
            break;
        }

        if (character == '.') {
            return index - 1;
        }
    }

    return path.ByteCount();
}

// 한 자리(base = ".../Content/Splash/<이름>")에서 확장자 순서대로 1x·@2x를 찾는다. 있으면 true.
Result<bool> FindAt(platform::FileSystem& files, StringView base, SplashImagePaths& out) noexcept {
    for (const StringView extension : SplashImageExtensions) {
        if (auto composed = ComposePath(out.standard, base, StringView(), extension); !composed) {
            return Result<bool>::Failure(composed.Error());
        }

        if (auto composed = ComposePath(out.doubled, base, SplashDoubleSuffix, extension);
            !composed) {
            return Result<bool>::Failure(composed.Error());
        }

        const bool hasStandard = Exists(files, out.standard);
        const bool hasDouble = Exists(files, out.doubled);
        if (hasStandard || hasDouble) {
            if (!hasStandard) {
                out.standard.Clear();
            }

            if (!hasDouble) {
                out.doubled.Clear();
            }

            return Result<bool>::Success(true);
        }
    }

    out.standard.Clear();
    out.doubled.Clear();
    return Result<bool>::Success(false);
}

// directory/Content/Splash/<name>이다. directory 끝 구분자는 하나로 맞춘다.
Result<void> ContentBase(String& out, StringView directory, StringView name) noexcept {
    out.Clear();
    if (auto appended = out.TryAppend(directory); !appended) {
        return appended;
    }

    const bool separator =
        !directory.IsEmpty() && (directory.Data()[directory.ByteCount() - 1] == '/' ||
                                 directory.Data()[directory.ByteCount() - 1] == '\\');
    if (!separator) {
        if (auto appended = out.TryAppend("/"); !appended) {
            return appended;
        }
    }

    if (auto appended = out.TryAppend(SplashContentFolder); !appended) {
        return appended;
    }

    return out.TryAppend(name);
}

} // namespace

StringView SplashImageName(SplashKind kind) noexcept {
    return kind == SplashKind::Studio ? StringView("StudioSplash") : StringView("Splash");
}

Result<SplashImagePaths> ResolveSplashImage(Allocator& allocator, platform::FileSystem& files,
                                            const SplashImageQuery& query) noexcept {
    SplashImagePaths paths(allocator);
    String base(allocator);
    if (!query.configuredPath.IsEmpty()) {
        // 설정 경로만 본다(UE SetCustomSplashImage와 같다 — 다른 자리로 넘어가지 않는다).
        String expanded(allocator);
        if (auto done = ExpandPath(expanded, query.configuredPath, query.projectDirectory,
                                   query.engineDirectory);
            !done) {
            return Result<SplashImagePaths>::Failure(done.Error());
        }

        const StringView view = expanded.View();
        const usize extension = ExtensionOffset(view);
        if (auto composed =
                ComposePath(paths.doubled, view.SliceBytes(0, extension), SplashDoubleSuffix,
                            view.SliceBytes(extension, view.ByteCount() - extension));
            !composed) {
            return Result<SplashImagePaths>::Failure(composed.Error());
        }

        const bool hasStandard = Exists(files, expanded);
        const bool hasDouble = Exists(files, paths.doubled);
        if (!hasStandard && !hasDouble) {
            return Result<SplashImagePaths>::Failure(ErrorCode::NotFound);
        }

        if (hasStandard) {
            paths.standard.Swap(expanded);
        }

        if (!hasDouble) {
            paths.doubled.Clear();
        }

        return Result<SplashImagePaths>::Success(Move(paths));
    }

    const StringView name = SplashImageName(query.kind);
    const StringView places[] = {query.projectDirectory, query.engineDirectory};
    for (const StringView directory : places) {
        if (directory.IsEmpty()) {
            continue;
        }

        if (auto composed = ContentBase(base, directory, name); !composed) {
            return Result<SplashImagePaths>::Failure(composed.Error());
        }

        auto found = FindAt(files, base.View(), paths);
        if (!found) {
            return Result<SplashImagePaths>::Failure(found.Error());
        }

        if (found.Value()) {
            return Result<SplashImagePaths>::Success(Move(paths));
        }
    }

    return Result<SplashImagePaths>::Failure(ErrorCode::NotFound);
}

Result<SplashSize> SplashLogicalSize(SplashSize standard, SplashSize doubled) noexcept {
    const bool hasStandard = standard.width != 0 || standard.height != 0;
    const bool hasDouble = doubled.width != 0 || doubled.height != 0;
    auto withinLimit = [](SplashSize size) noexcept {
        return size.width != 0 && size.height != 0 && size.width <= MaxSplashImageDimension &&
               size.height <= MaxSplashImageDimension;
    };
    if ((!hasStandard && !hasDouble) || (hasStandard && !withinLimit(standard)) ||
        (hasDouble && !withinLimit(doubled))) {
        return Result<SplashSize>::Failure(ErrorCode::InvalidArgument);
    }

    SplashSize logical = standard;
    if (hasStandard && hasDouble) {
        if (doubled.width != standard.width * 2 || doubled.height != standard.height * 2) {
            return Result<SplashSize>::Failure(ErrorCode::InvalidArgument);
        }
    } else if (hasDouble) {
        if (doubled.width % 2 != 0 || doubled.height % 2 != 0) {
            return Result<SplashSize>::Failure(ErrorCode::InvalidArgument);
        }

        logical = {doubled.width / 2, doubled.height / 2};
    }

    if (logical.width < MinSplashLogicalDimension || logical.height < MinSplashLogicalDimension) {
        return Result<SplashSize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<SplashSize>::Success(logical);
}

SplashImageVariant ChooseSplashVariant(double scale, bool hasStandard, bool hasDouble) noexcept {
    if (hasDouble && (!hasStandard || scale > NativeScale)) {
        return SplashImageVariant::Double;
    }

    return SplashImageVariant::Standard;
}

SplashSize SplashWindowSize(SplashSize logical, double scale) noexcept {
    if (!platform::PlatformMath::IsFinite(scale) || scale <= 0.0) {
        scale = NativeScale;
    }

    if (scale > MaxScale) {
        scale = MaxScale;
    }

    auto scaled = [scale](uint32 value) noexcept {
        const double result = platform::PlatformMath::Round(static_cast<double>(value) * scale);
        return result < 1.0 ? uint32{1} : static_cast<uint32>(result);
    };
    return {scaled(logical.width), scaled(logical.height)};
}

} // namespace rocks::splash
