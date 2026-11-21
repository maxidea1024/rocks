#include "AssetDatabaseInternal.h"

#include <AssetDatabase/ProjectLayout.h>
#include <Core/Containers/Array.h>
#include <Core/FileError.h>
#include <Core/Text/Path.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Path.h>

// 프로젝트 배치와 경로 대응이다(Docs/Design/AssetSystem.ko.md 4.2절, 7.1절 10 — A2a).
namespace rocks {

namespace {

constexpr StringView ContentName = "/Content";
constexpr StringView ConfigName = "/Config";
constexpr StringView IntermediateName = "/Intermediate";
constexpr StringView SavedName = "/Saved";

// 경로 임시 저장소의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize LayoutScratchBytes = 512;

Result<void> MakeChild(String& out, StringView root, StringView child) noexcept {
    String made(out.GetAllocator());
    if (auto appended = made.TryAppend(root); !appended) {
        return appended;
    }

    if (auto appended = made.TryAppend(child); !appended) {
        return appended;
    }

    out = Move(made);
    return Result<void>::Success();
}

} // namespace

ProjectLayout::ProjectLayout(Allocator& allocator) noexcept
    : allocator_(&allocator), root_(allocator), content_(allocator), config_(allocator),
      intermediate_(allocator), saved_(allocator) {
}

Result<void> ProjectLayout::Initialize(StringView root) noexcept {
    if (root.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 절대 경로로 바꾼다(UE FPaths::ConvertRelativePathToFull — 상대 경로는 현재 작업 디렉터리
    // 기준). Windows의 현재 드라이브 루트("/x")·드라이브 상대("C:x")는 뜻이 실행 상태에 달려
    // 거부한다.
    auto kind = Path::Root(root);
    if (!kind) {
        return Result<void>::Failure(kind.Error());
    }

    Result<String> absolute = Result<String>::Failure(ErrorCode::InvalidArgument);
    switch (kind.Value()) {
    case PathRoot::Relative: {
        Array<char> working(*allocator_);
        if (auto sized = working.TryResize(MaxNativePathBytes() + 1); !sized) {
            return sized;
        }

        usize bytes = 0;
        if (auto got = platform::GetStandardDirectory(platform::StandardDirectory::Working,
                                                      working.Data(), working.Size(), bytes);
            !got) {
            return Result<void>::Failure(FileErrorCode(got));
        }

        absolute = Path::MakeAbsolute(*allocator_, StringView(working.Data(), bytes), root);
        break;
    }
    case PathRoot::WindowsCurrentDriveRoot:
    case PathRoot::WindowsDriveRelative:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    default:
        absolute = Path::Normalize(*allocator_, root);
        break;
    }

    if (!absolute) {
        return Result<void>::Failure(absolute.Error());
    }

    // 정규화 결과의 구분자는 '/'다(Core Path — POSIX에서 '\'는 이름의 문자다). 루트뿐인
    // 경로("C:/"·"/")는 끝 '/'를 남기고 자식은 그 뒤에 붙인다.
    const StringView base = absolute.Value().View();
    const bool rootOnly = base.EndsWith("/");
    const StringView prefix(base.Data(), rootOnly ? base.ByteCount() - 1 : base.ByteCount());
    String newRoot(*allocator_);
    String newContent(*allocator_);
    String newConfig(*allocator_);
    String newIntermediate(*allocator_);
    String newSaved(*allocator_);
    if (auto made = newRoot.TryAppend(base); !made) {
        return made;
    }

    String* const outs[] = {&newContent, &newConfig, &newIntermediate, &newSaved};
    const StringView names[] = {ContentName, ConfigName, IntermediateName, SavedName};
    for (usize index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (auto made = MakeChild(*outs[index], prefix, names[index]); !made) {
            return made;
        }
    }

    root_ = Move(newRoot);
    content_ = Move(newContent);
    config_ = Move(newConfig);
    intermediate_ = Move(newIntermediate);
    saved_ = Move(newSaved);
    return Result<void>::Success();
}

Result<void> ProjectLayout::TryMakeFilePath(const LogicalPath& path, String& out) const noexcept {
    if (root_.View().IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize bytes = content_.View().ByteCount() + 1 + path.Relative().ByteCount() +
                        EditorPackageExtension.ByteCount();
    if (bytes + TemporaryFileSuffixBytes > MaxNativePathBytes()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    String made(out.GetAllocator());
    const StringView pieces[] = {content_.View(), StringView("/"), path.Relative(),
                                 EditorPackageExtension};
    for (StringView piece : pieces) {
        if (auto appended = made.TryAppend(piece); !appended) {
            return appended;
        }
    }

    out = Move(made);
    return Result<void>::Success();
}

Result<LogicalPath> ProjectLayout::TryMakeLogicalPath(StringView contentRelative,
                                                      LogicalPathFailure* failure) const noexcept {
    if (failure != nullptr) {
        *failure = LogicalPathFailure{};
    }

    // 확장자는 ASCII 대소문자를 무시한다(UE FPackageName::IsAssetPackageExtension — 대소문자를
    // 무시하는 파일 시스템에서 "Hero.RASSET"도 같은 패키지 자리다. 실제 이름은 색인이 가진다).
    const usize extension = EditorPackageExtension.ByteCount();
    if (contentRelative.ByteCount() <= extension ||
        !contentRelative.EndsWith(EditorPackageExtension, StringComparison::IgnoreAsciiCase)) {
        return Result<LogicalPath>::Failure(ErrorCode::InvalidArgument);
    }

    const StringView stem(contentRelative.Data(), contentRelative.ByteCount() - extension);
    StringBuilder<LayoutScratchBytes> text(*allocator_);
    const StringView pieces[] = {GameMount, "/", stem};
    if (auto appended = asset_database_detail::AppendAll(text, pieces); !appended) {
        return Result<LogicalPath>::Failure(appended.Error());
    }

    auto parsed = ParseLogicalPath(*allocator_, text.View(), failure);
    if (!parsed) {
        return parsed;
    }

    // 파싱은 마운트 뒤 철자를 NFC 정규화만 바꾼다. 바뀌었으면(비 NFC 파일 이름) byte 비교 파일
    // 시스템에서는 그 이름으로 같은 파일을 열 수 없다.
    if constexpr (!FileSystemNormalizesNames) {
        if (parsed.Value().Relative() != stem) {
            if (failure != nullptr) {
                failure->error = LogicalPathError::NotNfcOnDisk;
            }

            return Result<LogicalPath>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return parsed;
}

} // namespace rocks
