#include "AssetDatabaseInternal.h"

#include <AssetDatabase/ProjectLayout.h>
#include <Core/Text/StringBuilder.h>

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

    // '\'를 '/'로 바꾸고 끝 구분자를 뺀다(루트 하나뿐이면 남긴다).
    StringBuilder<LayoutScratchBytes> cleaned(*allocator_);
    for (char c : root) {
        const char separator = '/';
        const char text[1] = {c == '\\' ? separator : c};
        if (auto appended = cleaned.TryAppend(StringView(text, 1)); !appended) {
            return appended;
        }
    }

    usize length = cleaned.View().ByteCount();
    while (length > 1 && cleaned.View().Data()[length - 1] == '/') {
        --length;
    }

    const StringView base(cleaned.View().Data(), length);
    String newRoot(*allocator_);
    String newContent(*allocator_);
    String newConfig(*allocator_);
    String newIntermediate(*allocator_);
    String newSaved(*allocator_);
    if (auto made = newRoot.TryAppend(base); !made) {
        return made;
    }

    // 루트가 "/"면 자식은 "/Content"처럼 구분자를 겹치지 않는다.
    const StringView prefix = base == "/" ? StringView() : base;
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

    const usize extension = EditorPackageExtension.ByteCount();
    if (contentRelative.ByteCount() <= extension ||
        StringView(contentRelative.Data() + contentRelative.ByteCount() - extension, extension) !=
            EditorPackageExtension) {
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
