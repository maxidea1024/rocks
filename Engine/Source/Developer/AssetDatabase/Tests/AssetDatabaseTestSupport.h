#pragma once

#include <AssetDatabase/AssetClock.h>
#include <AssetDatabase/ProjectLayout.h>
#include <Core/Text/String.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// AssetDatabase 시험 공용 도구다(Docs/Design/AssetSystem.ko.md 7.1절 — A2).
namespace rocks::asset_database_test {

// 시험 프로젝트 디렉터리다(현재 디렉터리 아래 `rocks-asset-database-<tag>-<pid>` — 끝나면 지운다).
struct TempProject {
    explicit TempProject(const char* tag) noexcept {
        const int count = snprintf(root, sizeof(root), "rocks-asset-database-%s-%u", tag,
                                   static_cast<unsigned>(platform::CurrentProcessId()));
        rootBytes = count > 0 ? static_cast<usize>(count) : 0;
        made = rootBytes != 0 && rootBytes < sizeof(root) &&
               platform::MakeDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes);
    }

    ~TempProject() noexcept {
        if (made) {
            CHECK(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes));
        }
    }

    StringView Root() const noexcept {
        return StringView(root, rootBytes);
    }

    char root[96] = {};
    usize rootBytes = 0;
    bool made = false;
};

// 고정 시각을 주는 시계다(시험이 Advance로 옮긴다).
class ManualAssetClock final : public AssetClock {
public:
    explicit ManualAssetClock(int64 start) noexcept : now(start) {
    }

    int64 NowTicks() noexcept override {
        return now;
    }

    void Advance(int64 ticks) noexcept {
        now += ticks;
    }

    int64 now;
};

// Content 아래 relative에 bytes를 쓴다(부모 디렉터리를 만든다).
inline bool WriteContentFile(Allocator& allocator, const TempProject& project, StringView relative,
                             Span<const uint8> bytes) noexcept {
    String path(allocator);
    const StringView pieces[] = {project.Root(), "/Content/", relative};
    for (StringView piece : pieces) {
        if (!path.TryAppend(piece)) {
            return false;
        }
    }

    const StringView view = path.View();
    usize slash = view.ByteCount();
    while (slash > 0 && view.Data()[slash - 1] != '/') {
        --slash;
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    if (!platform::MakeDirectoryTree(files, view.Data(), slash - 1)) {
        return false;
    }

    platform::File file;
    if (!file.Open(files, view.Data(), view.ByteCount(), platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    const bool written = static_cast<bool>(file.WriteAll(bytes.Data(), bytes.Size()).status);
    return static_cast<bool>(file.Close()) && written;
}

// Content 아래 빈 디렉터리를 만든다.
inline bool MakeContentDirectory(Allocator& allocator, const TempProject& project,
                                 StringView relative) noexcept {
    String path(allocator);
    const StringView pieces[] = {project.Root(), "/Content/", relative};
    for (StringView piece : pieces) {
        if (!path.TryAppend(piece)) {
            return false;
        }
    }

    return static_cast<bool>(platform::MakeDirectoryTree(
        platform::PhysicalFileSystem(), path.View().Data(), path.View().ByteCount()));
}

} // namespace rocks::asset_database_test
