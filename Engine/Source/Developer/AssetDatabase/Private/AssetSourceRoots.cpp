#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetSourceRoots.h>
#include <AssetDatabase/ImportRecord.h>
#include <Core/Algorithms.h>
#include <Core/Text/Json.h>
#include <Core/Text/Path.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/Unicode.h>

namespace rocks {

namespace {

constexpr StringView RootsKey = "roots";
// JSON 노드 상한: 루트 객체·"roots" 객체와 루트마다 값 하나.
constexpr uint32 SourceRootsBaseNodes = 2;

template <class T>
Result<T> FailSource(AssetDiagnostic* failure, ErrorCode error, uint64 detail = 0,
                     AssetDiagnosticCode code = AssetDiagnosticCode::InvalidSource) noexcept {
    if (failure != nullptr) {
        *failure = AssetDiagnostic{};
        failure->code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        failure->error = error;
        failure->detail = detail;
    }

    return Result<T>::Failure(error);
}

// 루트 이름 규칙이다(1–MaxImportRootBytes byte의 `[A-Za-z0-9_]`).
bool IsRootName(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > MaxImportRootBytes) {
        return false;
    }

    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char c = name.Data()[index];
        const bool word =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!word) {
            return false;
        }
    }

    return true;
}

// text를 정규화한 절대 경로(`/` 구분)로 out에 쓴다. 상대 경로는 base(비면 거부) 기준이다.
// Windows 현재 드라이브 루트(`\x`)·드라이브 상대(`C:x`)는 InvalidArgument다.
Result<void> NormalizeAbsolute(Allocator& allocator, StringView text, StringView base,
                               String& out) noexcept {
    auto root = Path::Root(text);
    if (!root) {
        return Result<void>::Failure(root.Error());
    }

    Result<String> normalized = Result<String>::Failure(ErrorCode::InvalidArgument);
    switch (root.Value()) {
    case PathRoot::Relative:
        if (base.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        normalized = Path::Join(allocator, base, text);
        break;
    case PathRoot::PosixRoot:
    case PathRoot::WindowsDriveAbsolute:
    case PathRoot::WindowsUnc:
        normalized = Path::Normalize(allocator, text);
        break;
    case PathRoot::WindowsCurrentDriveRoot:
    case PathRoot::WindowsDriveRelative:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!normalized) {
        return Result<void>::Failure(normalized.Error());
    }

    if (Path::HostStyle() == PathStyle::Windows) {
        auto slashed = Path::NormalizeFilename(allocator, normalized.Value().View());
        if (!slashed) {
            return Result<void>::Failure(slashed.Error());
        }

        out = rocks::Move(slashed.Value());
        return Result<void>::Success();
    }

    out = rocks::Move(normalized.Value());
    return Result<void>::Success();
}

// 정규화한 `/` 구분 경로의 빈 아닌 성분 수다(`C:/a` → 2, `/a` → 1, `//server/share` → 2).
usize CountComponents(StringView path) noexcept {
    usize count = 0;
    bool inside = false;
    for (usize index = 0; index < path.ByteCount(); ++index) {
        const bool separator = path.Data()[index] == '/';
        count += !separator && !inside ? 1 : 0;
        inside = !separator;
    }

    return count;
}

// path의 대소문자 무시 키와 성분 수를 root에 채운다.
Result<void> FillKey(Allocator& allocator, AssetSourceRoot& root) noexcept {
    StringBuilder<MaxImportRelativePathBytes> key(allocator);
    if (auto made = TryMakeCaselessKey(root.path.View(), key); !made) {
        return made;
    }

    root.components = CountComponents(root.path.View());
    return root.key.TryAssign(key.View());
}

} // namespace

AssetSourceRoots::AssetSourceRoots(Allocator& allocator, const ProjectLayout& layout) noexcept
    : allocator_(&allocator), layout_(&layout), roots_(allocator) {
}

Result<void> AssetSourceRoots::Load(platform::FileSystem& files,
                                    AssetDiagnostic* failure) noexcept {
    roots_.Clear();
    Allocator& allocator = *allocator_;
    String path(allocator);
    const StringView pieces[] = {layout_->ConfigDirectory(), "/", AssetSourceRootsFileName};
    for (StringView piece : pieces) {
        if (auto appended = path.TryAppend(piece); !appended) {
            return FailSource<void>(failure, appended.Error());
        }
    }

    Array<uint8> bytes(allocator);
    bool existed = false;
    bool tooLarge = false;
    if (auto read = asset_database_detail::ReadWhole(
            files, path.View(), MaxAssetSourceRootsFileBytes, bytes, existed, tooLarge);
        !read) {
        return FailSource<void>(failure, read.Error(), 0, AssetDiagnosticCode::None);
    }

    if (tooLarge) {
        return FailSource<void>(failure, ErrorCode::CapacityExceeded);
    }

    Array<AssetSourceRoot> roots(allocator);
    bool hasProject = false;
    if (existed) {
        JsonDocument document(allocator);
        JsonReadOptions options;
        options.maxNodes = static_cast<uint32>(MaxAssetSourceRoots) + SourceRootsBaseNodes;
        const JsonError error = document.Parse(
            StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()), options);
        if (!error.Succeeded()) {
            // 노드 상한은 루트 수 상한이다(루트마다 노드 하나).
            ErrorCode code = ErrorCode::InvalidArgument;
            if (error.code == JsonErrorCode::OutOfMemory) {
                code = ErrorCode::OutOfMemory;
            } else if (error.code == JsonErrorCode::TooManyNodes) {
                code = ErrorCode::CapacityExceeded;
            }

            return FailSource<void>(failure, code,
                                    (static_cast<uint64>(error.position.line) << 32) |
                                        error.position.column);
        }

        const JsonValue top = document.Root();
        if (!top.IsObject() || top.Size() != 1 || top.KeyAt(0) != RootsKey ||
            !top.ValueAt(0).IsObject()) {
            return FailSource<void>(failure, ErrorCode::InvalidArgument);
        }

        // 루트 수는 노드 상한이 막는다(MaxAssetSourceRoots 넘으면 TooManyNodes).
        const JsonValue list = top.ValueAt(0);

        if (auto reserved = roots.TryReserve(list.Size() + 1); !reserved) {
            return FailSource<void>(failure, reserved.Error());
        }

        for (usize index = 0; index < list.Size(); ++index) {
            const StringView name = list.KeyAt(index);
            const JsonValue value = list.ValueAt(index);
            if (!IsRootName(name) || !value.IsString() || value.GetString().IsEmpty()) {
                return FailSource<void>(failure, ErrorCode::InvalidArgument, index + 1);
            }

            AssetSourceRoot root(allocator);
            if (auto named = root.name.TryAssign(name); !named) {
                return FailSource<void>(failure, named.Error());
            }

            if (auto normalized =
                    NormalizeAbsolute(allocator, value.GetString(), layout_->Root(), root.path);
                !normalized) {
                return FailSource<void>(failure, normalized.Error(), index + 1);
            }

            if (auto keyed = FillKey(allocator, root); !keyed) {
                return FailSource<void>(failure, keyed.Error(), index + 1);
            }

            hasProject = hasProject || name == ProjectSourceRootName;
            (void)roots.TryPushBack(rocks::Move(root));
        }
    }

    if (!hasProject) {
        AssetSourceRoot project(allocator);
        Result<void> step = project.name.TryAssign(ProjectSourceRootName);
        if (step) {
            step = project.path.TryAssign(layout_->Root());
        }

        if (step) {
            step = FillKey(allocator, project);
        }

        if (step) {
            step = roots.TryPushBack(rocks::Move(project));
        }

        if (!step) {
            return FailSource<void>(failure, step.Error());
        }
    }

    // 이름은 byte 그대로 기록·비교하므로 대소문자만 다른 두 이름(기본 Project 포함)은 모호하다.
    for (usize index = 0; index < roots.Size(); ++index) {
        for (usize other = 0; other < index; ++other) {
            if (EqualsIgnoringAsciiCase(roots[index].name.View(), roots[other].name.View())) {
                return FailSource<void>(failure, ErrorCode::InvalidArgument, index + 1);
            }
        }
    }

    roots.Sort([](const AssetSourceRoot& left, const AssetSourceRoot& right) noexcept {
        return StringView::CompareBytes(left.name.View(), right.name.View()) < 0;
    });
    roots_ = rocks::Move(roots);
    return Result<void>::Success();
}

Result<void> AssetSourceRoots::Locate(StringView osPath, AssetSourceLocation& out) const noexcept {
    Allocator& allocator = *allocator_;
    out.root.Clear();
    out.relative.Clear();
    out.absolute = false;
    String normalized(allocator);
    if (auto made = NormalizeAbsolute(allocator, osPath, {}, normalized); !made) {
        return made;
    }

    StringBuilder<MaxImportRelativePathBytes> key(allocator);
    if (auto made = TryMakeCaselessKey(normalized.View(), key); !made) {
        return made;
    }

    // 성분 단위 가장 긴 접두 루트(이름 순으로 보며 더 길 때만 바꾼다 — 같은 길이면 이름 순 첫째).
    const AssetSourceRoot* best = nullptr;
    for (const AssetSourceRoot& root : roots_) {
        const StringView rootKey = root.key.View();
        const StringView pathKey = key.View();
        if (pathKey.ByteCount() <= rootKey.ByteCount() || !pathKey.StartsWith(rootKey) ||
            (!rootKey.EndsWith("/") && pathKey.Data()[rootKey.ByteCount()] != '/')) {
            continue;
        }

        if (best == nullptr || root.components > best->components) {
            best = &root;
        }
    }

    Result<void> step = Result<void>::Success();
    if (best == nullptr) {
        out.absolute = true;
        step = out.relative.TryAssign(normalized.View());
    } else {
        // 루트 성분 수만큼 건너뛴 나머지 성분을 `/`로 잇는다(경로 길이만큼 먼저 잡아 잇기는
        // 실패하지 않는다).
        step = out.root.TryAssign(best->name.View());
        const StringView path = normalized.View();
        if (step) {
            step = out.relative.TryReserve(path.ByteCount());
        }

        usize seen = 0;
        usize start = 0;
        for (usize index = 0; step && index <= path.ByteCount(); ++index) {
            if (index < path.ByteCount() && path.Data()[index] != '/') {
                continue;
            }

            if (index > start) {
                ++seen;
                if (seen > best->components) {
                    if (!out.relative.IsEmpty()) {
                        (void)out.relative.TryAppend("/");
                    }

                    (void)out.relative.TryAppend(path.SliceBytes(start, index - start));
                }
            }

            start = index + 1;
        }
    }

    if (!step) {
        return step;
    }

    if (out.relative.ByteCount() > MaxImportRelativePathBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (!IsValidImportSourceLocation(out.root.View(), out.relative.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> AssetSourceRoots::Resolve(StringView root, StringView relative,
                                       String& out) const noexcept {
    out.Clear();
    if (!IsValidImportSourceLocation(root, relative)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (root.IsEmpty()) {
        return out.TryAssign(relative);
    }

    const usize at = algo::LowerBound(
        roots_.View(), root, [](const AssetSourceRoot& item, StringView probe) noexcept {
            return StringView::CompareBytes(item.name.View(), probe) < 0;
        });
    if (at == roots_.Size() || roots_[at].name.View() != root) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const StringView base = roots_[at].path.View();
    Result<void> step = out.TryAssign(base);
    if (step && !base.EndsWith("/")) {
        step = out.TryAppend("/");
    }

    if (step) {
        step = out.TryAppend(relative);
    }

    return step;
}

Result<void> ReadImportSource(platform::FileSystem& files, StringView path, Array<uint8>& bytes,
                              Sha256Digest& digest, uint64 limit,
                              AssetDiagnostic* failure) noexcept {
    bool existed = false;
    bool tooLarge = false;
    if (auto read = asset_database_detail::ReadWhole(files, path, limit, bytes, existed, tooLarge);
        !read) {
        return FailSource<void>(failure, read.Error());
    }

    if (!existed) {
        return FailSource<void>(failure, ErrorCode::NotFound, 0,
                                AssetDiagnosticCode::MissingSource);
    }

    if (tooLarge) {
        return FailSource<void>(failure, ErrorCode::CapacityExceeded);
    }

    digest = Sha256::Hash(bytes.View());
    return Result<void>::Success();
}

} // namespace rocks
