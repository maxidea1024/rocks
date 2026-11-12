#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <TestSupport/FileLink.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
using namespace rocks::platform;

// 저장 경로가 쓰는 File::Flush·FileSystem::Replace·SetModifiedTime·FileLock과 Windows 긴 경로를
// 실제 파일 시스템에서 검사한다(에셋 시스템 A0-P).
namespace {

struct Scratch {
    char path[200]{};
    usize length = 0;

    bool Make(const char* tag) noexcept {
        static int counter = 0;
        const int count = snprintf(path, sizeof(path), "rocks-durable-%s-%llu-%d", tag,
                                   static_cast<unsigned long long>(MonotonicNanoseconds()),
                                   AtomicFetchAdd<MemoryOrder::Relaxed>(&counter, 1) + 1);
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }
        length = static_cast<usize>(count);
        return static_cast<bool>(PhysicalFileSystem().MakeDirectory(path, length));
    }

    const char* Child(const char* relative, char* out, usize capacity) const noexcept {
        snprintf(out, capacity, "%s/%s", path, relative);
        return out;
    }

    ~Scratch() noexcept {
        if (length != 0) {
            (void)DeleteDirectoryTree(PhysicalFileSystem(), path, length);
        }
    }
};

bool WriteFile(const char* path, const char* content) noexcept {
    File file;
    if (!file.Open(PhysicalFileSystem(), path, strlen(path), FileOpenMode::CreateAlways)) {
        return false;
    }
    const auto written = file.WriteAll(content, strlen(content));
    const bool flushed = static_cast<bool>(file.Flush());
    return written.status && flushed && static_cast<bool>(file.Close());
}

bool ReadText(const char* path, char* buffer, usize capacity) noexcept {
    File file;
    if (!file.Open(PhysicalFileSystem(), path, strlen(path), FileOpenMode::ReadExisting)) {
        return false;
    }
    const auto read = file.Read(buffer, capacity - 1u);
    buffer[read.bytes] = '\0';
    return read.status && static_cast<bool>(file.Close());
}

} // namespace

TEST(FileDurability_FlushWritesAndRejectsClosedFile) {
    Scratch scratch;
    REQUIRE(scratch.Make("flush"));
    char path[256];
    REQUIRE(WriteFile(scratch.Child("a.bin", path, sizeof(path)), "payload"));
    File closed;
    CHECK(closed.Flush().code == FileCode::NotOpen);
    char text[32];
    REQUIRE(ReadText(path, text, sizeof(text)));
    CHECK(strcmp(text, "payload") == 0);
}

TEST(FileDurability_ReplaceSwapsContentAndTargetMayBeOpen) {
    Scratch scratch;
    REQUIRE(scratch.Make("replace"));
    char target[256];
    char temp[256];
    REQUIRE(WriteFile(scratch.Child("asset.rasset", target, sizeof(target)), "old"));
    REQUIRE(WriteFile(scratch.Child("asset.rasset.tmp", temp, sizeof(temp)), "new"));

    // 다른 쪽(백신·인덱서·다른 도구)이 대상을 열어 둔 상태에서도 교체한다.
    File reader;
    REQUIRE(reader.Open(PhysicalFileSystem(), target, strlen(target), FileOpenMode::ReadExisting));
    const FileStatus replaced =
        PhysicalFileSystem().Replace(temp, strlen(temp), target, strlen(target));
    CHECK(replaced);
    // 열어 둔 핸들은 예전 내용을 계속 읽는다.
    char old[16]{};
    const auto read = reader.Read(old, sizeof(old) - 1u);
    CHECK(read.status && strcmp(old, "old") == 0);
    REQUIRE(reader.Close());

    char text[32];
    REQUIRE(ReadText(target, text, sizeof(text)));
    CHECK(strcmp(text, "new") == 0);
    bool exists = true;
    REQUIRE(PhysicalFileSystem().Exists(temp, strlen(temp), exists));
    CHECK(!exists);

    // 대상이 없으면 이름만 바꾼다. 원본이 없으면 NotFound, 디렉터리는 WrongType이다.
    char fresh[256];
    REQUIRE(WriteFile(temp, "fresh"));
    scratch.Child("fresh.rasset", fresh, sizeof(fresh));
    CHECK(PhysicalFileSystem().Replace(temp, strlen(temp), fresh, strlen(fresh)));
    CHECK(PhysicalFileSystem().Replace(temp, strlen(temp), fresh, strlen(fresh)).code ==
          FileCode::NotFound);
    char directory[256];
    scratch.Child("dir", directory, sizeof(directory));
    REQUIRE(PhysicalFileSystem().MakeDirectory(directory, strlen(directory)));
    REQUIRE(WriteFile(temp, "x"));
    CHECK(PhysicalFileSystem().Replace(temp, strlen(temp), directory, strlen(directory)).code ==
          FileCode::WrongType);
    CHECK(PhysicalFileSystem().Replace(directory, strlen(directory), fresh, strlen(fresh)).code ==
          FileCode::WrongType);
}

TEST(FileDurability_ReplaceRejectsLinksAndReadOnlyTargets) {
    Scratch scratch;
    REQUIRE(scratch.Make("replace-link"));
    char real[256];
    char other[256];
    char link[256];
    char target[256];
    REQUIRE(WriteFile(scratch.Child("real.rasset", real, sizeof(real)), "real"));
    REQUIRE(WriteFile(scratch.Child("other.rasset", other, sizeof(other)), "other"));
    scratch.Child("link.rasset", link, sizeof(link));
    scratch.Child("target.rasset", target, sizeof(target));
    const test::FileLinkResult created = test::CreateFileLink(link, "real.rasset");
    if (created != test::FileLinkResult::Unavailable) {
        REQUIRE(created == test::FileLinkResult::Created);
        // 원본이 link면 link가 가리키는 파일을 옮기지 않고 거부한다(Windows·POSIX 같은 결과).
        CHECK(PhysicalFileSystem().Replace(link, strlen(link), target, strlen(target)).code ==
              FileCode::WrongType);
        bool exists = true;
        REQUIRE(PhysicalFileSystem().Exists(target, strlen(target), exists));
        CHECK(!exists);
        // 대상이 link면 link를 따라 교체하지 않는다.
        CHECK(PhysicalFileSystem().Replace(other, strlen(other), link, strlen(link)).code ==
              FileCode::WrongType);
        char text[16];
        REQUIRE(ReadText(real, text, sizeof(text)));
        CHECK(strcmp(text, "real") == 0);
    }
}

TEST(FileDurability_FlushOnReadOnlyHandleSucceeds) {
    Scratch scratch;
    REQUIRE(scratch.Make("flush-read"));
    char path[256];
    REQUIRE(WriteFile(scratch.Child("a.bin", path, sizeof(path)), "payload"));
    File reader;
    REQUIRE(reader.Open(PhysicalFileSystem(), path, strlen(path), FileOpenMode::ReadExisting));
    CHECK(reader.Flush());
    REQUIRE(reader.Close());
}

TEST(FileDurability_SetModifiedTimeIsVisibleThroughStat) {
    Scratch scratch;
    REQUIRE(scratch.Make("mtime"));
    char path[256];
    REQUIRE(WriteFile(scratch.Child("a.ddc", path, sizeof(path)), "x"));
    // 2030-01-01T00:00:00Z
    constexpr int64 Ticks2030 = 741077ll * 864000000000ll;
    REQUIRE(PhysicalFileSystem().SetModifiedTime(path, strlen(path), Ticks2030));
    FileInfo info;
    REQUIRE(PhysicalFileSystem().Stat(path, strlen(path), info));
    const int64 difference = info.modifiedTicks - Ticks2030;
    CHECK(difference > -2 * FileTicksPerSecond && difference < 2 * FileTicksPerSecond);
    // 범위: 1601-01-01 초과(Windows FILETIME 하한), 9999-12-31 이하. 모든 플랫폼이 같다.
    const int64 outsides[] = {0, FileTicksSettableAfter, FileTicksSettableLast + 1};
    for (const int64 outside : outsides) {
        CHECK(PhysicalFileSystem().SetModifiedTime(path, strlen(path), outside).code ==
              FileCode::InvalidArgument);
    }
    REQUIRE(PhysicalFileSystem().SetModifiedTime(path, strlen(path), FileTicksSettableAfter + 1));
    // 1601-01-01 직후는 파일 시스템 해상도에 따라 반올림될 수 있으므로 읽기 결과는 보지 않는다.
}

TEST(FileDurability_SyncDirectoryAcceptsDirectoriesOnly) {
    Scratch scratch;
    REQUIRE(scratch.Make("sync"));
    // 게시 순서: 패키지 파일을 Replace로 게시하고 그 디렉터리를 동기화한 뒤 manifest를 게시한다.
    char packages[256];
    scratch.Child("Packages", packages, sizeof(packages));
    REQUIRE(PhysicalFileSystem().MakeDirectory(packages, strlen(packages)));
    char temp[300];
    char package[300];
    snprintf(temp, sizeof(temp), "%s/a.rpak.tmp", packages);
    snprintf(package, sizeof(package), "%s/a.rpak", packages);
    REQUIRE(WriteFile(temp, "package"));
    REQUIRE(PhysicalFileSystem().Replace(temp, strlen(temp), package, strlen(package)));
    CHECK(PhysicalFileSystem().SyncDirectory(packages, strlen(packages)));
    CHECK(PhysicalFileSystem().SyncDirectory(scratch.path, scratch.length));
    // 끝 구분자도 디렉터리로 해석한다.
    char trailing[260];
    snprintf(trailing, sizeof(trailing), "%s/", packages);
    CHECK(PhysicalFileSystem().SyncDirectory(trailing, strlen(trailing)));
    // 현재 디렉터리(상대 경로 ".")도 된다.
    CHECK(PhysicalFileSystem().SyncDirectory(".", 1));

    // 파일은 끝 구분자가 있어도 WrongType(두 플랫폼 같은 결과), 중간 구성 요소가 파일이거나 없는
    // 경로는 NotFound, 빈·잘못된 경로는 InvalidArgument다.
    CHECK(PhysicalFileSystem().SyncDirectory(package, strlen(package)).code == FileCode::WrongType);
    char packageSlash[310];
    snprintf(packageSlash, sizeof(packageSlash), "%s/", package);
    CHECK(PhysicalFileSystem().SyncDirectory(packageSlash, strlen(packageSlash)).code ==
          FileCode::WrongType);
    char throughFile[310];
    snprintf(throughFile, sizeof(throughFile), "%s/inner", package);
    CHECK(PhysicalFileSystem().SyncDirectory(throughFile, strlen(throughFile)).code ==
          FileCode::NotFound);
    char missing[300];
    snprintf(missing, sizeof(missing), "%s/missing", packages);
    CHECK(PhysicalFileSystem().SyncDirectory(missing, strlen(missing)).code == FileCode::NotFound);
    CHECK(PhysicalFileSystem().SyncDirectory("", 0).code == FileCode::InvalidArgument);
    const char embedded[] = {'a', '\0', 'b'};
    CHECK(PhysicalFileSystem().SyncDirectory(embedded, sizeof(embedded)).code ==
          FileCode::InvalidArgument);

    // 디렉터리 link는 대상 디렉터리를 동기화하고, 파일 link는 WrongType이다.
    char directoryLink[300];
    snprintf(directoryLink, sizeof(directoryLink), "%s/packages-link", scratch.path);
    const test::FileLinkResult directoryCreated =
        test::CreateDirectoryLink(directoryLink, "Packages");
    if (directoryCreated != test::FileLinkResult::Unavailable) {
        REQUIRE(directoryCreated == test::FileLinkResult::Created);
        CHECK(PhysicalFileSystem().SyncDirectory(directoryLink, strlen(directoryLink)));
    }

    char fileLink[300];
    snprintf(fileLink, sizeof(fileLink), "%s/file-link", packages);
    const test::FileLinkResult fileCreated = test::CreateFileLink(fileLink, "a.rpak");
    if (fileCreated != test::FileLinkResult::Unavailable) {
        REQUIRE(fileCreated == test::FileLinkResult::Created);
        CHECK(PhysicalFileSystem().SyncDirectory(fileLink, strlen(fileLink)).code ==
              FileCode::WrongType);
    }
}

TEST(FileDurability_FileLockIsExclusiveUntilReleased) {
    Scratch scratch;
    REQUIRE(scratch.Make("lock"));
    char path[256];
    scratch.Child("AssetDatabase.lock", path, sizeof(path));
    FileLock first;
    REQUIRE(first.Acquire(path, 0));
    CHECK(first.IsHeld());
    CHECK(first.Acquire(path, 0).code == FileCode::AlreadyOpen);

    // 같은 프로세스의 다른 잠금도 배타다. 시간 안에 풀리지 않으면 Busy다.
    FileLock second;
    const uint64 start = MonotonicNanoseconds();
    CHECK(second.Acquire(path, 120).code == FileCode::Busy);
    CHECK(MonotonicNanoseconds() - start >= 100000000ull);
    CHECK(!second.IsHeld());

    first.Release();
    CHECK(!first.IsHeld());
    CHECK(second.Acquire(path, 0));
    second.Release();
}

TEST(FileDurability_WindowsLongPathsWork) {
    if (HostPathStyle() != PathStyle::Windows) {
        return; // POSIX는 PATH_MAX 안에서 원래 긴 경로를 쓴다.
    }
    Scratch scratch;
    REQUIRE(scratch.Make("long"));
    // MAX_PATH(260)를 넘는 깊이의 디렉터리와 파일을 만들고 읽고 열거하고 지운다.
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s", scratch.path);
    for (int level = 0; level < 8; ++level) {
        written += snprintf(path + written, sizeof(path) - static_cast<usize>(written),
                            "/directory-level-%02d-with-a-long-name", level);
    }
    REQUIRE(static_cast<usize>(written) > 300);
    REQUIRE(MakeDirectoryTree(PhysicalFileSystem(), path, static_cast<usize>(written)));
    char file[1100];
    snprintf(file, sizeof(file), "%s/content.rasset", path);
    REQUIRE(WriteFile(file, "deep"));
    char text[16];
    REQUIRE(ReadText(file, text, sizeof(text)));
    CHECK(strcmp(text, "deep") == 0);
    int entries = 0;
    REQUIRE(PhysicalFileSystem().EnumerateDirectory(
        path, static_cast<usize>(written),
        [](const DirectoryEntry&, void* context) noexcept {
            ++*static_cast<int*>(context);
            return true;
        },
        &entries));
    CHECK(entries == 1);
    char replacement[1100];
    snprintf(replacement, sizeof(replacement), "%s/content.rasset.tmp", path);
    REQUIRE(WriteFile(replacement, "newer"));
    CHECK(PhysicalFileSystem().Replace(replacement, strlen(replacement), file, strlen(file)));
    REQUIRE(ReadText(file, text, sizeof(text)));
    CHECK(strcmp(text, "newer") == 0);
}

TEST(FileDurability_WindowsShortRelativePathWithLongFullPathWorks) {
    if (HostPathStyle() != PathStyle::Windows) {
        return;
    }
    Scratch scratch;
    REQUIRE(scratch.Make("relative"));
    // 입력은 확장 임계값(240) 아래지만 현재 디렉터리를 붙이면 디렉터리 생성 한계(248)를 넘는
    // 상대 경로다. 판정이 입력 길이만 보면 ERROR_FILENAME_EXCED_RANGE로 실패한다.
    char path[256];
    int written = snprintf(path, sizeof(path), "%s/", scratch.path);
    while (written < 236) {
        // 40 byte마다 구분자를 넣어 구성 요소 길이 한계(255)를 지킨다.
        path[written] = written % 40 == 39 && path[written - 1] != '/' ? '/' : 'r';
        ++written;
    }
    path[written] = '\0';
    REQUIRE(path[written - 1] != '/');
    REQUIRE(MakeDirectoryTree(PhysicalFileSystem(), path, static_cast<usize>(written)));
    char file[300];
    snprintf(file, sizeof(file), "%s/f.bin", path);
    REQUIRE(WriteFile(file, "relative"));
    char text[16];
    REQUIRE(ReadText(file, text, sizeof(text)));
    CHECK(strcmp(text, "relative") == 0);
}
