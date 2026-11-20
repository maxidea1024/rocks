#pragma once

#include <AssetDatabase/AssetClock.h>
#include <AssetDatabase/ProjectLayout.h>
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

} // namespace rocks::asset_database_test
