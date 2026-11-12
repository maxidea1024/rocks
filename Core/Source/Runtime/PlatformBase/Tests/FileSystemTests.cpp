#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <TestSupport/FileLink.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
using namespace rocks::platform;

// 실제 OS 파일 시스템으로 디렉터리·Stat·열거·Copy/Move·재귀 helper·기본 경로를 검사한다.
namespace {

// 0001-01-01부터 2020-01-01, 2100-01-01까지의 tick이다(시각이 그럴듯한지만 본다).
constexpr int64 Ticks2020 = 737424ll * 864000000000ll;
constexpr int64 Ticks2100 = 766644ll * 864000000000ll;

struct Scratch {
    char path[200]{};
    usize length = 0;

    bool Make(const char* tag) noexcept {
        static int counter = 0;
        const int count = snprintf(path, sizeof(path), "rocks-fs-%s-%llu-%d", tag,
                                   static_cast<unsigned long long>(MonotonicNanoseconds()),
                                   __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }
        length = static_cast<usize>(count);
        return true;
    }

    // root 아래 상대 경로를 만든다.
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

usize Length(const char* text) noexcept {
    return strlen(text);
}

bool WriteFile(const char* path, const char* content) noexcept {
    File file;
    if (!file.Open(PhysicalFileSystem(), path, Length(path), FileOpenMode::CreateAlways)) {
        return false;
    }
    const auto written = file.WriteAll(content, Length(content));
    return written.status && static_cast<bool>(file.Close());
}

bool ReadFile(const char* path, char* buffer, usize capacity, usize& bytes) noexcept {
    File file;
    if (!file.Open(PhysicalFileSystem(), path, Length(path), FileOpenMode::ReadExisting)) {
        return false;
    }
    const auto read = file.Read(buffer, capacity);
    bytes = read.bytes;
    return read.status && static_cast<bool>(file.Close());
}

struct Listing {
    int files = 0;
    int directories = 0;
    int links = 0;
    int visits = 0;
    int stopAfter = 1000;
    bool sawA = false;
    bool sawSub = false;
    uint64 aSize = 0;
};

bool Collect(const DirectoryEntry& entry, void* raw) noexcept {
    auto& listing = *static_cast<Listing*>(raw);
    ++listing.visits;
    if (entry.info.isLink) {
        ++listing.links;
    } else if (entry.info.type == FileType::Directory) {
        ++listing.directories;
    } else if (entry.info.type == FileType::File) {
        ++listing.files;
    }
    if (entry.nameBytes == 5 && memcmp(entry.name, "a.bin", 5) == 0 && entry.name[5] == '\0') {
        listing.sawA = true;
        listing.aSize = entry.info.size;
    }
    if (entry.nameBytes == 3 && memcmp(entry.name, "sub", 3) == 0) {
        listing.sawSub = true;
    }
    return listing.visits < listing.stopAfter;
}

struct NameProbe {
    const char* name = nullptr;
    bool found = false;
    int visits = 0;
};

// 대소문자까지 같은 이름이 있는지 본다.
bool FindName(const DirectoryEntry& entry, void* raw) noexcept {
    auto& probe = *static_cast<NameProbe*>(raw);
    ++probe.visits;
    if (entry.nameBytes == Length(probe.name) &&
        memcmp(entry.name, probe.name, entry.nameBytes) == 0) {
        probe.found = true;
    }
    return true;
}

// 순수 가상 함수만 구현한 wrapper다. 확장 연산은 기본 구현(Unsupported)을 쓴다.
class MinimalFileSystem final : public FileSystem {
public:
    FileStatus Open(const char*, usize, FileOpenMode, FileHandle*&) noexcept override {
        return {FileCode::Unsupported, 0};
    }
    FileStatus Exists(const char*, usize, bool& exists) noexcept override {
        exists = false;
        return {};
    }
    FileStatus Delete(const char*, usize) noexcept override {
        return {FileCode::Unsupported, 0};
    }
};

} // namespace

TEST(FileSystem_DirectoriesStatAndEnumeration) {
    FileSystem& system = PhysicalFileSystem();
    Scratch root;
    REQUIRE(root.Make("dirs"));
    REQUIRE(system.MakeDirectory(root.path, root.length));
    CHECK(system.MakeDirectory(root.path, root.length).code == FileCode::AlreadyExists);
    FileInfo info;
    REQUIRE(system.Stat(root.path, root.length, info));
    CHECK(info.type == FileType::Directory && !info.isLink && info.size == 0);
    CHECK(info.modifiedTicks > Ticks2020 && info.modifiedTicks < Ticks2100);

    char a[256];
    char sub[256];
    char deep[256];
    root.Child("a.bin", a, sizeof(a));
    root.Child("sub", sub, sizeof(sub));
    root.Child("sub/deep/er", deep, sizeof(deep));
    REQUIRE(WriteFile(a, "hello"));
    REQUIRE(MakeDirectoryTree(system, deep, Length(deep)));
    CHECK(MakeDirectoryTree(system, deep, Length(deep))); // 이미 있으면 성공
    REQUIRE(system.Stat(a, Length(a), info));
    CHECK(info.type == FileType::File && info.size == 5 && !info.readOnly);
    CHECK(info.modifiedTicks > Ticks2020 && info.modifiedTicks < Ticks2100);
    CHECK(system.Stat(deep, Length(deep), info) && info.type == FileType::Directory);

    Listing listing;
    REQUIRE(system.EnumerateDirectory(root.path, root.length, Collect, &listing));
    CHECK(listing.visits == 2 && listing.files == 1 && listing.directories == 1);
    CHECK(listing.sawA && listing.sawSub && listing.aSize == 5);
    Listing early;
    early.stopAfter = 1;
    CHECK(system.EnumerateDirectory(root.path, root.length, Collect, &early));
    CHECK(early.visits == 1);

    // 종류 오류와 없는 경로.
    char missing[256];
    root.Child("missing", missing, sizeof(missing));
    CHECK(system.Stat(missing, Length(missing), info).code == FileCode::NotFound);
    CHECK(system.EnumerateDirectory(a, Length(a), Collect, &listing).code == FileCode::WrongType);
    CHECK(system.EnumerateDirectory(missing, Length(missing), Collect, &listing).code ==
          FileCode::NotFound);
    CHECK(system.EnumerateDirectory(root.path, root.length, nullptr, nullptr).code ==
          FileCode::InvalidArgument);
    CHECK(system.DeleteDirectory(root.path, root.length).code == FileCode::DirectoryNotEmpty);
    CHECK(system.DeleteDirectory(a, Length(a)).code == FileCode::WrongType);
    CHECK(system.Delete(sub, Length(sub)).code == FileCode::WrongType);
    // 경로 중간에 파일이 있으면 재귀 생성은 WrongType이다.
    char throughFile[256];
    root.Child("a.bin/x", throughFile, sizeof(throughFile));
    const auto blocked = MakeDirectoryTree(system, throughFile, Length(throughFile));
    CHECK(!blocked);

    // 재귀 삭제.
    REQUIRE(DeleteDirectoryTree(system, sub, Length(sub)));
    CHECK(system.Stat(sub, Length(sub), info).code == FileCode::NotFound);
    CHECK(DeleteDirectoryTree(system, a, Length(a)).code == FileCode::WrongType);
    REQUIRE(system.Delete(a, Length(a)));
    REQUIRE(system.DeleteDirectory(root.path, root.length));
    CHECK(system.Stat(root.path, root.length, info).code == FileCode::NotFound);
}

TEST(FileSystem_CopyAndMoveReplaceRules) {
    FileSystem& system = PhysicalFileSystem();
    Scratch root;
    REQUIRE(root.Make("copy"));
    REQUIRE(system.MakeDirectory(root.path, root.length));
    char a[256];
    char b[256];
    char c[256];
    char dir[256];
    char moved[256];
    root.Child("a.txt", a, sizeof(a));
    root.Child("b.txt", b, sizeof(b));
    root.Child("c.txt", c, sizeof(c));
    root.Child("dir", dir, sizeof(dir));
    root.Child("renamed", moved, sizeof(moved));
    REQUIRE(WriteFile(a, "alpha"));
    REQUIRE(WriteFile(b, "bravo!"));

    // Copy: 새 대상, 이미 있으면 AlreadyExists, replace면 덮어쓴다.
    REQUIRE(system.Copy(a, Length(a), c, Length(c), false));
    CHECK(system.Copy(a, Length(a), b, Length(b), false).code == FileCode::AlreadyExists);
    char buffer[16];
    usize bytes = 0;
    REQUIRE(ReadFile(b, buffer, sizeof(buffer), bytes));
    CHECK(bytes == 6 && memcmp(buffer, "bravo!", 6) == 0);
    REQUIRE(system.Copy(a, Length(a), b, Length(b), true));
    REQUIRE(ReadFile(b, buffer, sizeof(buffer), bytes));
    CHECK(bytes == 5 && memcmp(buffer, "alpha", 5) == 0);
    REQUIRE(system.MakeDirectory(dir, Length(dir)));
    CHECK(system.Copy(dir, Length(dir), moved, Length(moved), false).code == FileCode::WrongType);
    char missing[256];
    root.Child("missing", missing, sizeof(missing));
    CHECK(system.Copy(missing, Length(missing), moved, Length(moved), false).code ==
          FileCode::NotFound);

    // Move: 이름 변경, 이미 있으면 AlreadyExists, replace면 덮어쓴다. 디렉터리도 옮긴다.
    CHECK(system.Move(c, Length(c), b, Length(b), false).code == FileCode::AlreadyExists);
    REQUIRE(system.Move(c, Length(c), b, Length(b), true));
    FileInfo info;
    CHECK(system.Stat(c, Length(c), info).code == FileCode::NotFound);
    REQUIRE(system.Move(dir, Length(dir), moved, Length(moved), false));
    CHECK(system.Stat(moved, Length(moved), info) && info.type == FileType::Directory);
    CHECK(system.Stat(dir, Length(dir), info).code == FileCode::NotFound);

    // 자기 자신으로의 복사는 원본을 비우지 않고 실패한다.
    CHECK(!system.Copy(a, Length(a), a, Length(a), true));
    CHECK(system.Copy(a, Length(a), a, Length(a), false).code == FileCode::AlreadyExists);
    REQUIRE(ReadFile(a, buffer, sizeof(buffer), bytes));
    CHECK(bytes == 5 && memcmp(buffer, "alpha", 5) == 0);

    // 디렉터리는 replace여도 기존 대상을 덮어쓰지 않는다. 파일·디렉터리가 섞이면 WrongType이다.
    char other[256];
    root.Child("other", other, sizeof(other));
    REQUIRE(system.MakeDirectory(other, Length(other)));
    CHECK(system.Move(moved, Length(moved), other, Length(other), true).code ==
          FileCode::AlreadyExists);
    CHECK(system.Move(a, Length(a), other, Length(other), true).code == FileCode::WrongType);
    CHECK(system.Move(other, Length(other), a, Length(a), true).code == FileCode::WrongType);
    CHECK(system.Stat(moved, Length(moved), info) && info.type == FileType::Directory);
    CHECK(system.Stat(other, Length(other), info) && info.type == FileType::Directory);
    REQUIRE(ReadFile(a, buffer, sizeof(buffer), bytes));
    CHECK(bytes == 5 && memcmp(buffer, "alpha", 5) == 0);

    // 대소문자만 바꾸는 이름 변경은 대소문자를 구분하지 않는 volume에서도 된다.
    char upper[256];
    root.Child("Renamed", upper, sizeof(upper));
    REQUIRE(system.Move(moved, Length(moved), upper, Length(upper), false));
    NameProbe probe;
    probe.name = "Renamed";
    REQUIRE(system.EnumerateDirectory(root.path, root.length, FindName, &probe));
    CHECK(probe.found);
    char upperFile[256];
    root.Child("A.txt", upperFile, sizeof(upperFile));
    REQUIRE(system.Move(a, Length(a), upperFile, Length(upperFile), false));
    probe = {};
    probe.name = "A.txt";
    REQUIRE(system.EnumerateDirectory(root.path, root.length, FindName, &probe));
    CHECK(probe.found);

    // 파일 경로 끝에 구분자를 붙이면 Stat은 실패한다.
    char slashed[256];
    snprintf(slashed, sizeof(slashed), "%s/", upperFile);
    CHECK(!system.Stat(slashed, Length(slashed), info));
}

TEST(FileSystem_DeleteTreeDoesNotFollowDirectoryLinks) {
    FileSystem& system = PhysicalFileSystem();
    Scratch outside;
    Scratch tree;
    REQUIRE(outside.Make("target"));
    REQUIRE(tree.Make("tree"));
    REQUIRE(system.MakeDirectory(outside.path, outside.length));
    REQUIRE(system.MakeDirectory(tree.path, tree.length));
    char kept[256];
    outside.Child("keep.txt", kept, sizeof(kept));
    REQUIRE(WriteFile(kept, "keep"));
    char link[256];
    tree.Child("link", link, sizeof(link));
    // link 대상은 link가 있는 디렉터리 기준 상대 경로다.
    char target[256];
    // Windows 디렉터리 symlink는 대상에 '\' 구분자가 필요하다.
    snprintf(target, sizeof(target), "..%c%s", NativePathSeparator(), outside.path);
    const auto outcome = test::CreateDirectoryLink(link, target);
    if (outcome == test::FileLinkResult::Unavailable) {
        printf("[SKIP] directory link unavailable on this host\n");
        return;
    }
    REQUIRE(outcome == test::FileLinkResult::Created);
    FileInfo info;
    REQUIRE(system.Stat(link, Length(link), info));
    CHECK(info.isLink && info.type == FileType::Directory);
    Listing listing;
    REQUIRE(system.EnumerateDirectory(tree.path, tree.length, Collect, &listing));
    CHECK(listing.links == 1);
    // 재귀 삭제는 link 자체만 지우고 대상 디렉터리 내용은 남긴다.
    REQUIRE(DeleteDirectoryTree(system, tree.path, tree.length));
    CHECK(system.Stat(tree.path, tree.length, info).code == FileCode::NotFound);
    CHECK(system.Stat(kept, Length(kept), info) && info.type == FileType::File);

    // link를 루트로 넘기면 끝 구분자가 있어도 link만 지운다.
    REQUIRE(system.MakeDirectory(tree.path, tree.length));
    REQUIRE(test::CreateDirectoryLink(link, target) == test::FileLinkResult::Created);
    char slashed[256];
    snprintf(slashed, sizeof(slashed), "%s%c%c", link, NativePathSeparator(),
             NativePathSeparator());
    // 마지막 구성 요소가 "."·".."이면 플랫폼마다 해석이 달라지므로 아무것도 지우지 않고 거부한다.
    const char* dotted[] = {"%s/.", "%s/./", "%s/..", "%s/../"};
    for (const char* format : dotted) {
        char path[256];
        snprintf(path, sizeof(path), format, link);
        CHECK(DeleteDirectoryTree(system, path, Length(path)).code == FileCode::InvalidArgument);
        snprintf(path, sizeof(path), format, outside.path);
        CHECK(DeleteDirectoryTree(system, path, Length(path)).code == FileCode::InvalidArgument);
    }
    CHECK(system.Stat(kept, Length(kept), info) && info.type == FileType::File);
    CHECK(system.Stat(link, Length(link), info) && info.isLink);

    // link는 대상과 무관하게 디렉터리가 아니므로 디렉터리를 link 위로 옮기면 WrongType이다.
    char movable[256];
    tree.Child("movable", movable, sizeof(movable));
    REQUIRE(system.MakeDirectory(movable, Length(movable)));
    CHECK(system.Move(movable, Length(movable), link, Length(link), true).code ==
          FileCode::WrongType);
    REQUIRE(system.DeleteDirectory(movable, Length(movable)));

    // link를 거쳐 같은 파일로 복사하면 원본을 비우지 않고 거부한다.
    char fileLink[256];
    tree.Child("file-link", fileLink, sizeof(fileLink));
    char fileTarget[256];
    snprintf(fileTarget, sizeof(fileTarget), "..%c%s%ckeep.txt", NativePathSeparator(),
             outside.path, NativePathSeparator());
    REQUIRE(test::CreateFileLink(fileLink, fileTarget) == test::FileLinkResult::Created);
    CHECK(system.Copy(kept, Length(kept), fileLink, Length(fileLink), true).code ==
          FileCode::InvalidArgument);
    CHECK(system.Copy(fileLink, Length(fileLink), kept, Length(kept), true).code ==
          FileCode::InvalidArgument);
    char content[16];
    usize contentBytes = 0;
    REQUIRE(ReadFile(kept, content, sizeof(content), contentBytes));
    CHECK(contentBytes == 4 && memcmp(content, "keep", 4) == 0);
    REQUIRE(system.Delete(fileLink, Length(fileLink)));

    REQUIRE(DeleteDirectoryTree(system, slashed, Length(slashed)));
    CHECK(system.Stat(kept, Length(kept), info) && info.type == FileType::File);
    Listing afterSlash;
    REQUIRE(system.EnumerateDirectory(tree.path, tree.length, Collect, &afterSlash));
    CHECK(afterSlash.visits == 0);

    // 대상이 없는 link는 Stat이 NotFound지만 루트로 넘기면 link를 지운다.
    char dangling[256];
    tree.Child("dangling", dangling, sizeof(dangling));
    char nowhere[256];
    snprintf(nowhere, sizeof(nowhere), "..%c%s-missing", NativePathSeparator(), outside.path);
    REQUIRE(test::CreateDirectoryLink(dangling, nowhere) == test::FileLinkResult::Created);
    CHECK(system.Stat(dangling, Length(dangling), info).code == FileCode::NotFound);
    REQUIRE(DeleteDirectoryTree(system, dangling, Length(dangling)));
    Listing afterDangling;
    REQUIRE(system.EnumerateDirectory(tree.path, tree.length, Collect, &afterDangling));
    CHECK(afterDangling.visits == 0);
    CHECK(DeleteDirectoryTree(system, dangling, Length(dangling)).code == FileCode::NotFound);

    // 파일 link가 디렉터리를 가리켜도(Windows에서는 따라갈 수 없는 종류 불일치 link) 재귀 삭제는
    // link만 지운다. 루트로 직접 넘기면 Windows는 Stat이 실패하므로 아무것도 지우지 않고 실패한다.
    char mismatched[256];
    tree.Child("mismatched", mismatched, sizeof(mismatched));
    REQUIRE(test::CreateFileLink(mismatched, target) == test::FileLinkResult::Created);
    if (!DeleteDirectoryTree(system, mismatched, Length(mismatched))) {
        CHECK(HostPathStyle() == PathStyle::Windows);
    }
    CHECK(system.Stat(kept, Length(kept), info) && info.type == FileType::File);
    (void)test::CreateFileLink(mismatched, target); // 지워졌으면 다시 만든다
    REQUIRE(DeleteDirectoryTree(system, tree.path, tree.length));
    CHECK(system.Stat(tree.path, tree.length, info).code == FileCode::NotFound);
    CHECK(system.Stat(kept, Length(kept), info) && info.type == FileType::File);
}

TEST(FileSystem_WindowsDriveRelativePathIsNotDriveRoot) {
    if (HostPathStyle() != PathStyle::Windows) {
        return;
    }
    // "C:"는 그 드라이브의 현재 디렉터리다. 작업 디렉터리에 만든 고유 디렉터리가 보여야 하며
    // 드라이브 루트를 열거하면 안 된다.
    FileSystem& system = PhysicalFileSystem();
    char working[4096];
    usize workingBytes = 0;
    if (!GetStandardDirectory(StandardDirectory::Working, working, sizeof(working), workingBytes) ||
        workingBytes <= 3 || working[1] != ':') {
        // UNC·드라이브 루트 작업 디렉터리에서는 루트와 현재 디렉터리를 구별할 수 없다.
        printf("[SKIP] working directory is not a drive subdirectory\n");
        return;
    }
    Scratch marker;
    REQUIRE(marker.Make("drive"));
    REQUIRE(system.MakeDirectory(marker.path, marker.length));
    const char drive[2] = {working[0], ':'};
    NameProbe probe;
    probe.name = marker.path;
    REQUIRE(system.EnumerateDirectory(drive, 2, FindName, &probe));
    CHECK(probe.found);
}

namespace {

// 루트(POSIX "/", Windows "C:\")만 끝 구분자를 가질 수 있다.
bool IsRootPath(const char* path, usize bytes) noexcept {
    return (bytes == 1 && path[0] == '/') ||
           (bytes == 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
}

void CheckStandardDirectory(StandardDirectory kind) noexcept {
    char buffer[1024];
    usize bytes = 0;
    REQUIRE(GetStandardDirectory(kind, buffer, sizeof(buffer), bytes));
    CHECK(bytes > 0 && buffer[bytes] == '\0' && Length(buffer) == bytes);
    CHECK(IsRootPath(buffer, bytes) || (buffer[bytes - 1] != '/' && buffer[bytes - 1] != '\\'));
    FileInfo info;
    REQUIRE(PhysicalFileSystem().Stat(buffer, bytes, info));
    CHECK(info.type == FileType::Directory);
    // 버퍼가 부족하면 Overflow와 필요한 길이(NUL 제외)를 돌려준다.
    char small[1];
    usize required = 0;
    const auto status = GetStandardDirectory(kind, small, sizeof(small), required);
    CHECK(status.code == FileCode::Overflow && required == bytes);
}

} // namespace

TEST(FileSystem_StandardDirectories) {
    const StandardDirectory common[] = {StandardDirectory::Executable, StandardDirectory::Working,
                                        StandardDirectory::Temporary, StandardDirectory::Home};
    for (StandardDirectory kind : common) {
        CheckStandardDirectory(kind);
    }
    // desktop은 OS 기본 디렉터리를 쓰므로 등록이 Unsupported이고 사용자 디렉터리가 바로 있다.
    // 모바일(Android)은 앱이 등록하기 전에는 NotFound이고, 등록한 뒤에는 그 경로를 돌려준다.
    const auto registerProbe = RegisterApplicationDirectories(nullptr, nullptr);
    char buffer[1024];
    usize bytes = 0;
    if (GetStandardDirectory(StandardDirectory::UserData, buffer, sizeof(buffer), bytes)) {
        CHECK(registerProbe.code == FileCode::Unsupported);
        CheckStandardDirectory(StandardDirectory::UserData);
        CheckStandardDirectory(StandardDirectory::UserCache);
        return;
    }
    CHECK(registerProbe.code == FileCode::InvalidArgument);
    CHECK(GetStandardDirectory(StandardDirectory::UserCache, buffer, sizeof(buffer), bytes).code ==
          FileCode::NotFound);
    // 상대 경로는 등록할 수 없다.
    CHECK(RegisterApplicationDirectories("files", "cache").code == FileCode::InvalidArgument);
    // 작업 디렉터리 아래에 앱 디렉터리를 만들어 등록한다(실제 앱은 OS가 준 경로를 넘긴다).
    char working[512];
    usize workingBytes = 0;
    REQUIRE(
        GetStandardDirectory(StandardDirectory::Working, working, sizeof(working), workingBytes));
    Scratch data;
    Scratch cache;
    REQUIRE(data.Make("appdata") && cache.Make("appcache"));
    REQUIRE(PhysicalFileSystem().MakeDirectory(data.path, data.length));
    REQUIRE(PhysicalFileSystem().MakeDirectory(cache.path, cache.length));
    char dataPath[768];
    char cachePath[768];
    snprintf(dataPath, sizeof(dataPath), "%s/%s", working, data.path);
    snprintf(cachePath, sizeof(cachePath), "%s/%s", working, cache.path);
    REQUIRE(RegisterApplicationDirectories(dataPath, cachePath));
    REQUIRE(GetStandardDirectory(StandardDirectory::UserData, buffer, sizeof(buffer), bytes));
    CHECK(bytes == Length(dataPath) && memcmp(buffer, dataPath, bytes) == 0);
    REQUIRE(GetStandardDirectory(StandardDirectory::UserCache, buffer, sizeof(buffer), bytes));
    CHECK(bytes == Length(cachePath) && memcmp(buffer, cachePath, bytes) == 0);
    CheckStandardDirectory(StandardDirectory::UserData);
    CheckStandardDirectory(StandardDirectory::UserCache);
    // 등록 뒤 Temporary는 cache, Home은 data를 가리킨다.
    REQUIRE(GetStandardDirectory(StandardDirectory::Temporary, buffer, sizeof(buffer), bytes));
    CHECK(bytes == Length(cachePath) && memcmp(buffer, cachePath, bytes) == 0);
}

TEST(FileSystem_WrapperDefaultsAreUnsupported) {
    MinimalFileSystem minimal;
    FileInfo info;
    CHECK(minimal.Stat("x", 1, info).code == FileCode::Unsupported);
    CHECK(minimal.MakeDirectory("x", 1).code == FileCode::Unsupported);
    CHECK(minimal.DeleteDirectory("x", 1).code == FileCode::Unsupported);
    CHECK(minimal.EnumerateDirectory("x", 1, Collect, nullptr).code == FileCode::Unsupported);
    CHECK(minimal.Move("x", 1, "y", 1, false).code == FileCode::Unsupported);
    CHECK(minimal.Copy("x", 1, "y", 1, false).code == FileCode::Unsupported);
    CHECK(MakeDirectoryTree(minimal, "x/y", 3).code == FileCode::Unsupported);
    CHECK(DeleteDirectoryTree(minimal, "x", 1).code == FileCode::Unsupported);
}
