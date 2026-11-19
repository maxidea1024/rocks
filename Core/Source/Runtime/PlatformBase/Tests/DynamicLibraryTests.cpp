#include <Base/Compiler.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/DynamicLibrary.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <Platform/Process.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/DynamicLibraryFixture.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace dynamic_library_tests_file {

using namespace rocks::platform;
using namespace rocks::test;

// 동적 라이브러리 계약(Platform/DynamicLibrary.h)을 실제 OS 로더로 검사한다. OS 공급 라이브러리
// 시험은 모든 플랫폼에서 돌고, 의존 DLL·현재 디렉터리·PATH 시험은 시험 지원 코드가 실행 중에 쓴
// 데이터 전용 라이브러리(TestSupport/DynamicLibraryFixture.h)로 한다. 그 라이브러리를 만들 수 없는
// 플랫폼(현재 macOS·Android)에서는 SKIP을 출력한다.
namespace {

constexpr uint32 ValueA = 0xA11CE5A5u;
constexpr uint32 ValueB = 0xB0B0CAFEu;
constexpr uint32 ProbeTimeout = 30'000;

// 시험마다 다른 디렉터리·라이브러리 이름을 쓴다. Windows 로더는 같은 기본 이름의 DLL이 이미 올라와
// 있으면 디렉터리와 관계없이 그것을 쓰므로, 이름이 겹치면 앞 시험이 뒤 시험의 결과를 바꾼다.
uint32 NextSerial() noexcept {
    static uint32 serial = 0;
    return AtomicFetchAdd<MemoryOrder::Relaxed>(&serial, 1u) + 1u;
}

// 현재 디렉터리 아래의 절대 경로 작업 디렉터리다(Android는 앞선 시험이 임시 디렉터리 등록을 지운
// 경로로 남겨 두므로 다른 PlatformBase 시험처럼 현재 디렉터리를 쓴다). 소멸 때 지운다(Windows는
// 올라온 DLL을 지울 수 없으므로 시험은 그 전에 모든 라이브러리를 닫는다).
struct Scratch {
    char path[1024]{};
    usize length = 0;

    bool Make(const char* tag) noexcept {
        char base[512];
        usize bytes = 0;
        if (!GetStandardDirectory(StandardDirectory::Working, base, sizeof(base), bytes)) {
            return false;
        }

        const int count =
            snprintf(path, sizeof(path), "%s%crocks-dynlib-%s-%llu-%u", base, NativePathSeparator(),
                     tag, static_cast<unsigned long long>(MonotonicNanoseconds()), NextSerial());
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }

        length = static_cast<usize>(count);
        if (!MakeDirectoryTree(PhysicalFileSystem(), path, length)) {
            length = 0;
            return false;
        }

        return true;
    }

    // 이 디렉터리 아래 name의 절대 경로를 out에 쓴다.
    bool Child(const char* name, char* out, usize capacity) const noexcept {
        const int count = snprintf(out, capacity, "%s%c%s", path, NativePathSeparator(), name);
        return count > 0 && static_cast<usize>(count) < capacity;
    }

    ~Scratch() noexcept {
        if (length != 0) {
            (void)DeleteDirectoryTree(PhysicalFileSystem(), path, length);
        }
    }
};

bool WriteText(const char* path, const char* content) noexcept {
    File file;
    if (!file.Open(PhysicalFileSystem(), path, strlen(path), FileOpenMode::CreateAlways)) {
        return false;
    }

    const auto written = file.WriteAll(content, strlen(content));
    return written.status && static_cast<bool>(file.Close());
}

// 고유한 시험 라이브러리 파일 이름이다(예: "RocksDynB17.dll").
void LibraryName(const char* stem, uint32 serial, char* out, usize capacity) noexcept {
    snprintf(out, capacity, "RocksDyn%s%u%s", stem, serial, DynamicLibraryExtension());
}

// 의존 쌍을 만든다: B는 RocksValue(ValueB)를 export하고, A는 RocksValue(ValueA)를 export하며 B의
// RocksValue를 import해 그 import 칸을 RocksImport로 export한다.
struct FixturePair {
    char nameA[64]{};
    char nameB[64]{};

    explicit FixturePair(uint32 serial, const char* stemA = "A", const char* stemB = "B") noexcept {
        LibraryName(stemA, serial, nameA, sizeof(nameA));
        LibraryName(stemB, serial, nameB, sizeof(nameB));
    }

    bool WriteA(const char* path) const noexcept {
        const FixtureExport exports[] = {{"RocksValue", ValueA}};
        FixtureLibrary library;
        library.exports = exports;
        library.exportCount = 1;
        library.importLibrary = nameB;
        library.importSymbol = "RocksValue";
        library.importSlotExport = "RocksImport";
        return WriteFixtureLibrary(path, library);
    }

    bool WriteB(const char* path) const noexcept {
        const FixtureExport exports[] = {{"RocksValue", ValueB}};
        FixtureLibrary library;
        library.exports = exports;
        library.exportCount = 1;
        return WriteFixtureLibrary(path, library);
    }
};

bool SkipWithoutFixtures(const char* test) noexcept {
    if (DynamicLibraryFixturesSupported()) {
        return false;
    }

    printf("[SKIP] %s: fixture libraries are not generated on this platform\n", test);
    return true;
}

uint32 ReadValue(const void* address) noexcept {
    uint32 value = 0;
    memcpy(&value, address, sizeof(value));
    return value;
}

} // namespace

TEST(DynamicLibraryRejectsInvalidArguments) {
    DynamicLibrary library;
    CHECK(library.Open(nullptr).code == FileCode::InvalidArgument);
    CHECK(library.Open("").code == FileCode::InvalidArgument);
    // 상대 경로는 현재 디렉터리에 따라 다른 파일을 가리키므로 받지 않는다.
    CHECK(library.Open("relative.dll").code == FileCode::InvalidArgument);
    CHECK(library.Open("sub/relative.so").code == FileCode::InvalidArgument);
    CHECK(library.Open("/abs\0olute", 9).code == FileCode::InvalidArgument);
    CHECK(library.Open("/\xc3\x28", 3).code == FileCode::InvalidArgument);
    CHECK(library.Open("/x", 0).code == FileCode::InvalidArgument);

    // OpenSystem은 파일 이름 하나만 받는다(다른 디렉터리를 가리킬 수 없다).
    CHECK(library.OpenSystem(nullptr).code == FileCode::InvalidArgument);
    CHECK(library.OpenSystem("").code == FileCode::InvalidArgument);
    CHECK(library.OpenSystem("dir/name.so").code == FileCode::InvalidArgument);
    CHECK(library.OpenSystem("dir\\name.dll").code == FileCode::InvalidArgument);
    CHECK(library.OpenSystem("C:name.dll").code == FileCode::InvalidArgument);
    CHECK(library.OpenSystem(".").code == FileCode::InvalidArgument);
    CHECK(library.OpenSystem("..").code == FileCode::InvalidArgument);
    char longName[DynamicLibrary::MaxSystemNameBytes + 2];
    memset(longName, 'a', sizeof(longName) - 1u);
    longName[sizeof(longName) - 1u] = '\0';
    CHECK(library.OpenSystem(longName).code == FileCode::InvalidArgument);
    CHECK(!library.IsOpen());

    // 닫힌 객체: Find는 NotOpen, Close는 할 일이 없어 Success다.
    DynamicLibraryLease lease;
    void* address = &lease;
    CHECK(library.Find("anything", address, lease).code == FileCode::NotOpen);
    CHECK(address == nullptr);
    CHECK(!lease.IsHeld());
    CHECK(library.LeaseCount() == 0);
    CHECK(library.Close());
    CHECK(library.Close());
}

TEST(DynamicLibraryReportsMissingFileDirectoryAndNonLibrary) {
    Scratch scratch;
    REQUIRE(scratch.Make("errors"));

    char missing[1100];
    REQUIRE(scratch.Child("missing-library.bin", missing, sizeof(missing)));
    DynamicLibrary library;
    CHECK(library.Open(missing).code == FileCode::NotFound);

    char missingDirectory[1100];
    REQUIRE(scratch.Child("no-such-dir/library.bin", missingDirectory, sizeof(missingDirectory)));
    CHECK(library.Open(missingDirectory).code == FileCode::NotFound);

    // 디렉터리와 라이브러리가 아닌 파일은 WrongType이다(OS 로더가 형식을 거부한다).
    CHECK(library.Open(scratch.path).code == FileCode::WrongType);
    char text[1100];
    REQUIRE(scratch.Child("not-a-library.bin", text, sizeof(text)));
    REQUIRE(WriteText(text, "this is not a shared library\n"));
    CHECK(library.Open(text).code == FileCode::WrongType);
    CHECK(!library.IsOpen());

    // OS 로더 문구: POSIX dlopen은 숫자 코드가 없으므로 dlerror 문구를 넘긴다. Windows는
    // nativeError가 원래 코드이고 문구는 비어 있다. 앞 호출의 문구는 다음 호출 시작에 지워진다.
    DynamicLibraryError error;
    error.truncated = true;
    const FileStatus rejected = library.Open(text, strlen(text), &error);
    CHECK(rejected.code == FileCode::WrongType);
    CHECK(!error.truncated);
    if (ROCKS_PLATFORM_WINDOWS) {
        CHECK(error.text[0] == '\0');
        CHECK(rejected.nativeError != 0);
    } else {
        CHECK(error.text[0] != '\0');
        CHECK(strlen(error.text) < DynamicLibraryError::MaxTextBytes);
    }

    CHECK(library.Open("relative.dll", strlen("relative.dll"), &error).code ==
          FileCode::InvalidArgument);
    CHECK(error.text[0] == '\0');

    // OS 공급 라이브러리에 없는 이름이다.
    CHECK(library.OpenSystem("rocks-no-such-system-library.bin").code == FileCode::NotFound);
    CHECK(!library.IsOpen());
}

TEST(DynamicLibraryFindsSymbolsAndCountsLeases) {
    char path[1024];
    const char* systemName = nullptr;
    const char* symbol = nullptr;
    REQUIRE(SystemLibraryForTest(path, sizeof(path), systemName, symbol));

    DynamicLibrary library;
    REQUIRE(library.Open(path));
    CHECK(library.IsOpen());
    CHECK(library.Open(path).code == FileCode::AlreadyOpen);
    CHECK(library.OpenSystem(systemName).code == FileCode::AlreadyOpen);

    DynamicLibraryLease first;
    void* address = nullptr;
    REQUIRE(library.Find(symbol, address, first));
    CHECK(address != nullptr);
    CHECK(first.IsHeld());
    CHECK(library.LeaseCount() == 1);

    // 없는 심볼은 NotFound이고 lease를 바꾸지 않는다.
    DynamicLibraryLease untouched;
    void* missingAddress = &untouched;
    CHECK(library.Find("RocksNoSuchSymbol", missingAddress, untouched).code == FileCode::NotFound);
    CHECK(missingAddress == nullptr);
    CHECK(!untouched.IsHeld());
    CHECK(library.LeaseCount() == 1);

    // 표 하나에 없는 심볼이 섞이면 앞서 찾은 주소까지 되돌린다(반쯤 채운 표가 남지 않는다).
    DynamicSymbol table[] = {{symbol, nullptr}, {"RocksNoSuchSymbol", nullptr}, {symbol, nullptr}};
    usize missingIndex = 0;
    CHECK(library.Find(table, 3, untouched, &missingIndex).code == FileCode::NotFound);
    CHECK(missingIndex == 1);
    CHECK(table[0].address == nullptr && table[1].address == nullptr &&
          table[2].address == nullptr);
    CHECK(!untouched.IsHeld());
    CHECK(library.LeaseCount() == 1);

    // 잘못된 표는 NotFound보다 먼저 InvalidArgument다.
    DynamicSymbol invalid[] = {{symbol, nullptr}, {"", nullptr}};
    CHECK(library.Find(invalid, 2, untouched, &missingIndex).code == FileCode::InvalidArgument);
    CHECK(missingIndex == IndexNone);
    CHECK(library.Find(nullptr, 1, untouched).code == FileCode::InvalidArgument);
    CHECK(library.Find(table, 0, untouched).code == FileCode::InvalidArgument);

    DynamicSymbol good[] = {{symbol, nullptr}, {symbol, nullptr}};
    DynamicLibraryLease second;
    REQUIRE(library.Find(good, 2, second, &missingIndex));
    CHECK(missingIndex == IndexNone);
    CHECK(good[0].address == address && good[1].address == address);
    CHECK(library.LeaseCount() == 2);

    // 같은 lease 객체로 다시 찾으면 새 lease를 얻은 뒤 이전 것을 놓는다(수는 그대로).
    REQUIRE(library.Find(good, 2, second));
    CHECK(library.LeaseCount() == 2);

    // lease가 남아 있으면 Close는 Busy이고 열린 상태 그대로다.
    CHECK(library.Close().code == FileCode::Busy);
    CHECK(library.IsOpen());
    first.Reset();
    CHECK(library.Close().code == FileCode::Busy);

    // 이동한 lease도 같은 보증이다.
    DynamicLibraryLease moved(static_cast<DynamicLibraryLease&&>(second));
    CHECK(!second.IsHeld());
    CHECK(moved.IsHeld());
    CHECK(library.LeaseCount() == 1);
    moved.Reset();
    CHECK(library.LeaseCount() == 0);
    CHECK(library.Close());
    CHECK(!library.IsOpen());

    // OS 공급 라이브러리를 이름으로 연다.
    REQUIRE(library.OpenSystem(systemName));
    DynamicLibraryLease named;
    void* namedAddress = nullptr;
    CHECK(library.Find(symbol, namedAddress, named));
    CHECK(namedAddress == address);
    named.Reset();

    // 이동 대입은 대상의 이전 라이브러리를 놓고 원본을 받는다.
    DynamicLibrary other;
    REQUIRE(other.Open(path));
    other = static_cast<DynamicLibrary&&>(library);
    CHECK(!library.IsOpen());
    CHECK(other.IsOpen());
    CHECK(other.Close());
}

namespace {

constexpr usize FindThreads = 8;
constexpr usize FindsPerThread = 64;

struct FindRace {
    const DynamicLibrary* library = nullptr;
    const char* symbol = nullptr;
    ManualResetEvent start;
    DynamicLibraryLease leases[FindThreads][FindsPerThread];
    uint32 failures = 0;
};

struct FindWorker {
    FindRace* race = nullptr;
    usize index = 0;
};

void RunFinds(void* context) noexcept {
    auto& worker = *static_cast<FindWorker*>(context);
    FindRace& race = *worker.race;
    (void)race.start.Wait();
    for (usize i = 0; i < FindsPerThread; ++i) {
        void* address = nullptr;
        if (!race.library->Find(race.symbol, address, race.leases[worker.index][i]) ||
            address == nullptr) {
            AtomicFetchAdd<MemoryOrder::Relaxed>(&race.failures, 1u);
        }
    }
}

} // namespace

// 여러 스레드가 같은 라이브러리에서 동시에 찾아도 lease 수가 정확하다(늘리기는 비교 교환). 모든
// 스레드가 한 gate를 기다렸다가 함께 시작하며, 결과는 Join 뒤에 수로 단언한다.
TEST(DynamicLibraryConcurrentFindCountsEveryLease) {
    char path[1024];
    const char* systemName = nullptr;
    const char* symbol = nullptr;
    REQUIRE(SystemLibraryForTest(path, sizeof(path), systemName, symbol));

    DynamicLibrary library;
    REQUIRE(library.Open(path));
    static FindRace race;
    race.library = &library;
    race.symbol = symbol;
    race.failures = 0;
    race.start.Reset();

    FindWorker workers[FindThreads];
    Thread threads[FindThreads];
    for (usize i = 0; i < FindThreads; ++i) {
        workers[i] = {&race, i};
        REQUIRE(threads[i].Start(&RunFinds, &workers[i]) == ThreadStatus::Success);
    }

    race.start.Signal();
    for (Thread& thread : threads) {
        CHECK(thread.Join() == ThreadStatus::Success);
    }

    CHECK(race.failures == 0);
    CHECK(library.LeaseCount() == FindThreads * FindsPerThread);
    CHECK(library.Close().code == FileCode::Busy);
    for (auto& row : race.leases) {
        for (DynamicLibraryLease& lease : row) {
            lease.Reset();
        }
    }

    CHECK(library.LeaseCount() == 0);
    CHECK(library.Close());
}

TEST(DynamicLibraryResolvesSameDirectoryDependency) {
    if (SkipWithoutFixtures("DynamicLibraryResolvesSameDirectoryDependency")) {
        return;
    }

    Scratch scratch;
    REQUIRE(scratch.Make("dependency"));
    const FixturePair pair(NextSerial());
    char pathA[1100];
    char pathB[1100];
    REQUIRE(scratch.Child(pair.nameA, pathA, sizeof(pathA)));
    REQUIRE(scratch.Child(pair.nameB, pathB, sizeof(pathB)));
    REQUIRE(pair.WriteA(pathA));
    REQUIRE(pair.WriteB(pathB));

    {
        DynamicLibrary libraryA;
        REQUIRE(libraryA.Open(pathA));
        CHECK(LibraryIsLoaded(pair.nameB));

        DynamicSymbol symbols[] = {{"RocksImport", nullptr}, {"RocksValue", nullptr}};
        DynamicLibraryLease leaseA;
        REQUIRE(libraryA.Find(symbols, 2, leaseA));
        CHECK(ReadValue(symbols[1].address) == ValueA);

        // A의 import 칸은 로더가 같은 디렉터리의 B에서 찾은 RocksValue 주소다.
        DynamicLibrary libraryB;
        REQUIRE(libraryB.Open(pathB));
        DynamicLibraryLease leaseB;
        void* valueB = nullptr;
        REQUIRE(libraryB.Find("RocksValue", valueB, leaseB));
        CHECK(ReadValue(valueB) == ValueB);
        void* imported = nullptr;
        memcpy(&imported, symbols[0].address, sizeof(imported));
        CHECK(imported == valueB);

        leaseB.Reset();
        CHECK(libraryB.Close());
        // A가 B를 아직 참조하므로 B는 남아 있다.
        CHECK(LibraryIsLoaded(pair.nameB));
        leaseA.Reset();
        CHECK(libraryA.Close());
    }

    CHECK(!LibraryIsLoaded(pair.nameA));
    CHECK(!LibraryIsLoaded(pair.nameB));

    // 의존 B가 없는 디렉터리의 A는 열지 못한다(NotFound). 실패해도 A가 남지 않는다.
    Scratch alone;
    REQUIRE(alone.Make("missing-dependency"));
    const FixturePair lonely(NextSerial());
    char lonelyA[1100];
    REQUIRE(alone.Child(lonely.nameA, lonelyA, sizeof(lonelyA)));
    REQUIRE(lonely.WriteA(lonelyA));
    DynamicLibrary library;
    CHECK(library.Open(lonelyA).code == FileCode::NotFound);
    CHECK(!library.IsOpen());
    CHECK(!LibraryIsLoaded(lonely.nameA));
}

// 소유자가 lease보다 먼저 사라지면 라이브러리는 마지막 lease가 풀릴 때 내려간다. 그 사이 주소는
// 계속 유효하다.
TEST(DynamicLibraryOwnerDestroyedBeforeLeaseKeepsLibrary) {
    if (SkipWithoutFixtures("DynamicLibraryOwnerDestroyedBeforeLeaseKeepsLibrary")) {
        return;
    }

    Scratch scratch;
    REQUIRE(scratch.Make("owner"));
    const FixturePair pair(NextSerial());
    char pathB[1100];
    REQUIRE(scratch.Child(pair.nameB, pathB, sizeof(pathB)));
    REQUIRE(pair.WriteB(pathB));

    DynamicLibraryLease lease;
    void* value = nullptr;
    {
        DynamicLibrary library;
        REQUIRE(library.Open(pathB));
        REQUIRE(library.Find("RocksValue", value, lease));
    }

    CHECK(LibraryIsLoaded(pair.nameB));
    CHECK(ReadValue(value) == ValueB);
    lease.Reset();
    CHECK(!LibraryIsLoaded(pair.nameB));
}

namespace {

// 경로 길이 경계다: 239·240(파일 backend가 확장 경로 접두사 "\\?\"를 붙이기 시작하는 240),
// 247·248(디렉터리 생성 한도 248), 259·260·261(MAX_PATH 260), 그리고 그보다 긴 경로.
constexpr usize LongPathTargets[] = {239, 240, 247, 248, 259, 260, 261, 300, 512};

// 작업 디렉터리 길이와 관계없이 마지막에 시험하는 길이다(기준 경로 + 이만큼, 언제나 260 초과).
constexpr usize LongPathMargin = 300;

constexpr usize LongPathCapacity = 2048;

constexpr usize MaxSegmentBytes = 64;

// root 아래에 디렉터리 이름 조각을 붙여 "<directory>/<fileBytes byte 파일 이름>"의 길이가 정확히
// target byte가 되게 한다. 붙일 길이(구분자 포함)를 MaxSegmentBytes 이하 조각으로 나누며 마지막에
// 1(구분자만)이 남지 않게 앞 조각을 한 글자 줄인다. 붙일 길이가 정확히 1이면 만들 수 없어 false다.
bool PadDirectory(const char* root, usize target, usize fileBytes, char* directory,
                  usize capacity) noexcept {
    usize length = strlen(root);
    if (length + 1u + fileBytes > target || target >= capacity) {
        return false;
    }

    usize remaining = target - length - 1u - fileBytes;
    if (remaining == 1u) {
        return false;
    }

    memcpy(directory, root, length);
    while (remaining != 0) {
        usize segment = remaining - 1u > MaxSegmentBytes ? MaxSegmentBytes : remaining - 1u;
        if (remaining - 1u - segment == 1u) {
            --segment;
        }

        directory[length++] = NativePathSeparator();
        memset(directory + length, 'd', segment);
        length += segment;
        remaining -= segment + 1u;
    }

    directory[length] = '\0';
    return true;
}

// 전체 경로가 정확히 target byte인 자리에 의존 쌍을 쓰고 A를 연다. 기준 경로가 target보다 길어
// 만들 수 없으면 checked를 false로 둔다(실패가 아니다).
void OpenAtPathLength(const Scratch& scratch, usize target, bool& checked) noexcept {
    checked = false;
    char root[LongPathCapacity];
    const int rootCount = snprintf(root, sizeof(root), "%s%c%llu", scratch.path,
                                   NativePathSeparator(), static_cast<unsigned long long>(target));
    REQUIRE(rootCount > 0 && static_cast<usize>(rootCount) < sizeof(root));

    // 붙일 길이가 1이면 파일 이름을 한 글자 늘려 같은 전체 길이를 만든다.
    const uint32 serial = NextSerial();
    FixturePair pair(serial);
    char directory[LongPathCapacity];
    if (!PadDirectory(root, target, strlen(pair.nameA), directory, sizeof(directory))) {
        pair = FixturePair(serial, "AX", "BX");
        if (!PadDirectory(root, target, strlen(pair.nameA), directory, sizeof(directory))) {
            return;
        }
    }

    REQUIRE(MakeDirectoryTree(PhysicalFileSystem(), directory, strlen(directory)));
    char pathA[LongPathCapacity];
    char pathB[LongPathCapacity];
    snprintf(pathA, sizeof(pathA), "%s%c%s", directory, NativePathSeparator(), pair.nameA);
    snprintf(pathB, sizeof(pathB), "%s%c%s", directory, NativePathSeparator(), pair.nameB);
    REQUIRE(strlen(pathA) == target);
    REQUIRE(pair.WriteA(pathA));
    REQUIRE(pair.WriteB(pathB));

    DynamicLibrary library;
    const FileStatus opened = library.Open(pathA);
    if (!opened) {
        printf("  long path %llu bytes: Open failed (code %d, native %u)\n",
               static_cast<unsigned long long>(target), static_cast<int>(opened.code),
               opened.nativeError);
    }

    REQUIRE(opened);
    DynamicLibraryLease lease;
    void* value = nullptr;
    REQUIRE(library.Find("RocksValue", value, lease));
    CHECK(ReadValue(value) == ValueA);
    lease.Reset();
    CHECK(library.Close());
    checked = true;
}

} // namespace

// MAX_PATH 경계 앞뒤와 그보다 긴 경로에서 같은 디렉터리 의존까지 연다. 경로는 작업 디렉터리 길이와
// 관계없이 정확한 전체 길이로 만든다(고정 개수의 디렉터리를 붙이면 짧은 작업 디렉터리에서 260자를
// 넘지 못한다). 기준 경로보다 짧은 경계 길이는 만들 수 없어 건너뛰지만, 기준 경로 + 300 길이는
// 언제나 시험한다.
TEST(DynamicLibraryOpensLongPath) {
    if (SkipWithoutFixtures("DynamicLibraryOpensLongPath")) {
        return;
    }

    Scratch scratch;
    REQUIRE(scratch.Make("long"));
    for (const usize target : LongPathTargets) {
        bool checked = false;
        OpenAtPathLength(scratch, target, checked);
    }

    bool checked = false;
    OpenAtPathLength(scratch, scratch.length + LongPathMargin, checked);
    CHECK(checked);
}

namespace {

// 이 실행 파일을 "--process-probe dynamic-library <how> <path> [dir]"로 작업 디렉터리
// workingDirectory에서 띄우고 종료 코드를 준다. 실행하지 못했거나 시간이 다했으면 1000이다.
uint32 RunDynamicLibraryProbe(const char* how, const char* path, const char* workingDirectory,
                              const char* pathDirectory) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return 1000;
    }

    ProcessArgument arguments[5] = {{"--process-probe", 15},
                                    {"dynamic-library", 15},
                                    {how, strlen(how)},
                                    {path, strlen(path)},
                                    {}};
    usize argumentCount = 4;
    if (pathDirectory != nullptr) {
        arguments[argumentCount++] = {pathDirectory, strlen(pathDirectory)};
    }

    ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = argumentCount;
    info.workingDirectory = workingDirectory;
    info.workingDirectoryBytes = strlen(workingDirectory);
    info.output = ProcessStreamMode::Null;
    info.error = ProcessStreamMode::Null;
    ProcessRunOptions options;
    options.timeoutMilliseconds = ProbeTimeout;
    const ProcessRunResult run = RunProcess(info, options);
    if (!run.status || run.timedOut || run.exit.kind != ProcessExitKind::Exited) {
        return 1000;
    }

    return run.exit.code;
}

} // namespace

// 음성 대조: 의존 DLL B를 현재 디렉터리나 PATH에만 두면 DynamicLibrary는 A를 열지 못한다(NotFound).
// 같은 자리에서 OS 기본 검색(대조군)은 A를 연다 — 즉 B가 실제로 찾을 수 있는 곳에 있는데도
// DynamicLibrary가 그곳을 보지 않는다. 시험 프로세스의 현재 디렉터리·환경을 바꾸지 않도록 자식
// probe에서 연다.
TEST(DynamicLibraryIgnoresWorkingDirectoryAndPath) {
    if (SkipWithoutFixtures("DynamicLibraryIgnoresWorkingDirectoryAndPath") ||
        !ProcessSpawnExpected()) {
        return;
    }

    Scratch libraryDirectory;
    Scratch dependencyDirectory;
    Scratch elsewhere;
    REQUIRE(libraryDirectory.Make("search-a"));
    REQUIRE(dependencyDirectory.Make("search-b"));
    REQUIRE(elsewhere.Make("search-cwd"));
    const FixturePair pair(NextSerial());
    char pathA[1100];
    char pathB[1100];
    REQUIRE(libraryDirectory.Child(pair.nameA, pathA, sizeof(pathA)));
    REQUIRE(dependencyDirectory.Child(pair.nameB, pathB, sizeof(pathB)));
    REQUIRE(pair.WriteA(pathA));
    REQUIRE(pair.WriteB(pathB));

    // B가 현재 디렉터리에만 있다.
    CHECK(RunDynamicLibraryProbe("default", pathA, dependencyDirectory.path, nullptr) == 0);
    CHECK(RunDynamicLibraryProbe("rocks", pathA, dependencyDirectory.path, nullptr) == 10);

    // B가 PATH에만 있다.
    CHECK(RunDynamicLibraryProbe("default", pathA, elsewhere.path, dependencyDirectory.path) == 0);
    CHECK(RunDynamicLibraryProbe("rocks", pathA, elsewhere.path, dependencyDirectory.path) == 10);

    // 양성 대조: B를 A 옆에 두면 같은 probe가 연다.
    char besideA[1100];
    REQUIRE(libraryDirectory.Child(pair.nameB, besideA, sizeof(besideA)));
    REQUIRE(pair.WriteB(besideA));
    CHECK(RunDynamicLibraryProbe("rocks", pathA, elsewhere.path, nullptr) == 0);
}
} // namespace dynamic_library_tests_file
