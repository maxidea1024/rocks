#include "AssetDatabaseTestSupport.h"

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/DirectoryChangeSource.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 쓰기 잠금·시계·감시 공급 시험이다(Docs/Design/AssetSystem 8절, 7.1절 4·9 — A2a). 다른
// 프로세스의 잠금은 자식 프로세스와 파이프 gate로 순서를 정한다(시간에 기대지 않는다).
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace asset_database_lock_tests_file {

using namespace rocks::asset_database_test;

namespace {

constexpr int ProbeBusyExit = 3;
constexpr int ProbeUsageExit = 2;
constexpr int ProbeFailureExit = 4;
// 자식이 잠금을 잡았다는 신호다(줄바꿈 없음 — Windows 텍스트 모드 stdout은 줄바꿈을 CR LF로
// 바꾼다).
constexpr char HeldSignal[] = "held";

// 자식: argv[3]의 프로젝트 잠금을 대기 없이 잡고 "held"를 쓴 뒤 표준 입력이 올(또는 닫힐) 때까지
// 쥔다. 못 잡으면 ProbeBusyExit다.
int LockProbe(int argc, const char* const* argv) noexcept {
    constexpr int RootArgument = 3;
    if (argc <= RootArgument) {
        return ProbeUsageExit;
    }

    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    if (!layout.Initialize(StringView(argv[RootArgument], strlen(argv[RootArgument])))) {
        return ProbeFailureExit;
    }

    AssetDatabaseLock lock;
    if (auto held = lock.Acquire(layout, 0); !held) {
        return held.Error() == ErrorCode::Busy ? ProbeBusyExit : ProbeFailureExit;
    }

    if (fwrite(HeldSignal, 1, sizeof(HeldSignal) - 1, stdout) != sizeof(HeldSignal) - 1 ||
        fflush(stdout) != 0) {
        return ProbeFailureExit;
    }

    char gate = 0;
    (void)fread(&gate, 1, 1, stdin);
    lock.Release();
    return 0;
}

const bool LockProbeRegistered =
    rocks::test::RegisterProcessProbe("asset-database-lock", &LockProbe);

// 잠금 자식을 시작한다(입력·출력 pipe).
bool StartLockChild(platform::Process& child, StringView root) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"asset-database-lock", 19}, {root.Data(), root.ByteCount()}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.input = platform::ProcessStreamMode::Pipe;
    info.output = platform::ProcessStreamMode::Pipe;
    info.error = platform::ProcessStreamMode::Null;
    return static_cast<bool>(child.Start(info));
}

// 자식이 "held"를 쓸 때까지 읽는다(자식이 끝나 EOF면 거짓).
bool WaitHeld(platform::Process& child) noexcept {
    char buffer[16] = {};
    usize received = 0;
    while (received < sizeof(HeldSignal) - 1) {
        const platform::FileTransfer read = child.Read(
            platform::ProcessStream::Output, buffer + received, sizeof(buffer) - 1 - received);
        if (!read.status || read.bytes == 0) {
            return false;
        }

        received += read.bytes;
    }

    return strncmp(buffer, HeldSignal, sizeof(HeldSignal) - 1) == 0;
}

int ExitCode(platform::Process& child) noexcept {
    if (child.Wait() != platform::WaitResult::Signaled) {
        return -1;
    }

    platform::ProcessExit exit;
    if (!child.GetExit(exit) || exit.kind != platform::ProcessExitKind::Exited) {
        return -1;
    }

    return static_cast<int>(exit.code);
}

} // namespace

// 같은 프로세스(8절): 잠금은 Intermediate를 만들고 그 아래 잠금 파일을 잡는다. 같은 객체로 다시
// 잡으면 AlreadyExists, 다른 객체는 Busy(대기 0)이고, 놓으면 다른 객체가 잡는다. IsHeldFor는 잡은
// 루트와 같은 배치에만 참이다(놓은 뒤·다른 루트·Initialize 전 거짓 — 7.1절 15 A2c2b).
TEST(AssetDatabaseLock_SameProcess) {
    TempProject project("lock-local");
    REQUIRE(project.made);
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize(project.Root()));
    AssetDatabaseLock first;
    REQUIRE(first.Acquire(layout, 0));
    CHECK(first.IsHeld());
    platform::FileInfo info;
    CHECK(platform::PhysicalFileSystem().Stat(layout.IntermediateDirectory().Data(),
                                              layout.IntermediateDirectory().ByteCount(), info) &&
          info.type == platform::FileType::Directory);
    CHECK(first.Acquire(layout, 0).Error() == ErrorCode::AlreadyExists);
    // 잡은 프로젝트 대조: 같은 루트의 다른 배치 객체는 참, 다른 루트·Initialize 전 배치는 거짓.
    ProjectLayout same(allocator);
    REQUIRE(same.Initialize(project.Root()));
    TempProject otherProject("lock-other");
    REQUIRE(otherProject.made);
    ProjectLayout other(allocator);
    REQUIRE(other.Initialize(otherProject.Root()));
    ProjectLayout uninitialized(allocator);
    CHECK(first.IsHeldFor(layout) && first.IsHeldFor(same));
    CHECK(!first.IsHeldFor(other) && !first.IsHeldFor(uninitialized));
    AssetDatabaseLock second;
    CHECK(!second.IsHeldFor(layout));
    CHECK(second.Acquire(layout, 0).Error() == ErrorCode::Busy);
    CHECK(!second.IsHeld());
    first.Release();
    CHECK(!first.IsHeld() && !first.IsHeldFor(layout));
    REQUIRE(second.Acquire(layout, 0));
    second.Release();
    // 놓은 뒤 다시 놓아도 아무것도 하지 않는다.
    second.Release();
    // 놓인 잠금은 대기가 있어도 바로 잡는다.
    REQUIRE(first.Acquire(layout, 60000));
    first.Release();
    // Initialize 전 배치는 잠그지 않는다.
    ProjectLayout empty(allocator);
    CHECK(second.Acquire(empty, 0).Error() == ErrorCode::InvalidArgument);
}

// 다른 프로세스(8절): 자식이 잡고 있으면 부모는 Busy, 자식이 gate를 받아 놓은 뒤에는 잡는다. 부모가
// 잡고 있으면 자식이 Busy로 끝난다.
TEST(AssetDatabaseLock_OtherProcess) {
    // Android는 자식 프로세스를 시작하지 않는다(platform::Process — Unsupported).
    if constexpr (IsAndroid) {
        printf("  skipped: no child processes on Android\n");
        return;
    }

    REQUIRE(LockProbeRegistered);
    TempProject project("lock-process");
    REQUIRE(project.made);
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize(project.Root()));
    {
        platform::Process child;
        REQUIRE(StartLockChild(child, project.Root()));
        REQUIRE(WaitHeld(child));
        AssetDatabaseLock lock;
        CHECK(lock.Acquire(layout, 0).Error() == ErrorCode::Busy);
        const char gate = 'x';
        REQUIRE(child.WriteInput(&gate, 1).status);
        CHECK(ExitCode(child) == 0);
        REQUIRE(lock.Acquire(layout, 0));
        // 부모가 쥔 동안 자식은 Busy로 끝난다.
        platform::Process blocked;
        REQUIRE(StartLockChild(blocked, project.Root()));
        CHECK(ExitCode(blocked) == ProbeBusyExit);
        lock.Release();
    }

    // 쥔 자식이 비정상 종료하면(Terminate) OS가 잠금을 풀어 부모가 대기 없이 잡는다(8절).
    {
        platform::Process child;
        REQUIRE(StartLockChild(child, project.Root()));
        REQUIRE(WaitHeld(child));
        AssetDatabaseLock lock;
        CHECK(lock.Acquire(layout, 0).Error() == ErrorCode::Busy);
        REQUIRE(child.Terminate());
        CHECK(child.Wait() == platform::WaitResult::Signaled);
        // OS는 끝난 프로세스의 잠금을 풀지만 그 시점은 정하지 않는다(LockFileEx — 시스템 자원에
        // 따름). 대기는 실패 상한으로만 쓴다.
        constexpr uint32 ReleaseWaitMilliseconds = 60000;
        CHECK(lock.Acquire(layout, ReleaseWaitMilliseconds));
    }
}

// 잠금 실패(8절): Intermediate 자리가 파일이면 디렉터리를 만들 수 없어 그 파일 오류이고 잡지
// 않는다. 잠금 파일 경로가 OS 상한을 넘으면 CapacityExceeded다.
TEST(AssetDatabaseLock_Failures) {
    TempProject project("lock-fail");
    REQUIRE(project.made);
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize(project.Root()));
    platform::File file;
    REQUIRE(file.Open(layout.IntermediateDirectory().Data(), platform::FileOpenMode::CreateNew));
    REQUIRE(file.Close());
    AssetDatabaseLock lock;
    const auto acquired = lock.Acquire(layout, 0);
    CHECK(!acquired && acquired.Error() == ErrorCode::InvalidArgument);
    CHECK(!lock.IsHeld());
    String root(allocator);
    while (root.View().ByteCount() < MaxNativePathBytes()) {
        REQUIRE(root.TryAppend("r"));
    }

    ProjectLayout tooLong(allocator);
    REQUIRE(tooLong.Initialize(root.View()));
    CHECK(lock.Acquire(tooLong, 0).Error() == ErrorCode::CapacityExceeded);
}

// 잠금 경로 할당 실패(8절): 프로젝트 경로가 길어(임시 저장소 512 byte를 넘는다) 잠금 파일 경로가
// 자라는 모든 할당 지점에서 한 번 실패하면 그 오류이고 잡지 않으며, 실패가 없으면 잡는다.
TEST(AssetDatabaseLock_AllocationFailures) {
    TempProject project("lock-alloc");
    REQUIRE(project.made);
    SystemAllocator system;
    String root(system);
    REQUIRE(root.TryAppend(project.Root()));
    constexpr uint32 NestedDirectories = 10;
    for (uint32 index = 0; index < NestedDirectories; ++index) {
        REQUIRE(root.TryAppend("/"));
        for (uint32 byte = 0; byte < MaxNameComponentBytes; ++byte) {
            REQUIRE(root.TryAppend("n"));
        }
    }

    test::NFailAllocator failing(system);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        ProjectLayout layout(failing);
        REQUIRE(layout.Initialize(root.View()));
        REQUIRE(layout.IntermediateDirectory().ByteCount() > 512);
        AssetDatabaseLock lock;
        failing.FailOnAttempt(attempt);
        const auto acquired = lock.Acquire(layout, 0);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(acquired && lock.IsHeld());
            break;
        }

        CHECK(!acquired && acquired.Error() == ErrorCode::OutOfMemory && !lock.IsHeld());
        ++failures;
    }

    CHECK(failures > 0);
}

// 시계(7.1절 9): 시스템 시계는 platform::UtcTicks와 같은 표현(2020년 이후 값)이고, 시험 시계는 준
// 시각을 그대로 돌려준다.
TEST(AssetClock_Sources) {
    SystemAssetClock system;
    // 2020-01-01 00:00:00 UTC의 tick이다(0001-01-01부터 100 ns).
    constexpr int64 Year2020Ticks = 637134336000000000;
    CHECK(system.NowTicks() > Year2020Ticks);
    ManualAssetClock manual(Year2020Ticks);
    CHECK(manual.NowTicks() == Year2020Ticks);
    manual.Advance(AssetClockTicksPerSecond);
    CHECK(manual.NowTicks() == Year2020Ticks + AssetClockTicksPerSecond);
    CHECK(AssetClockTicksPerMillisecond * 1000 == AssetClockTicksPerSecond);
}

namespace {

struct SeenChange {
    bool added = false;
};

bool RecordAdded(const platform::DirectoryChange& change, void* context) noexcept {
    auto* seen = static_cast<SeenChange*>(context);
    if (change.kind == platform::DirectoryChangeKind::Added &&
        StringView(change.path, change.pathBytes).EndsWith("watched.rasset")) {
        seen->added = true;
    }

    return true;
}

} // namespace

// OS 감시 공급(7.1절 9): 감시를 연 디렉터리에 만든 파일의 Added 이벤트가 Wait·Poll로 온다. 기다림은
// Wait 반복 횟수 상한으로만 끝난다(sleep 없음 — 상한을 넘으면 실패).
TEST(WatcherChangeSource_ReportsAddedFile) {
    TempProject project("watch");
    REQUIRE(project.made);
    WatcherChangeSource source;
    REQUIRE(source.Open(project.root, project.rootBytes));
    char path[160];
    const int count = snprintf(path, sizeof(path), "%s/watched.rasset", project.root);
    REQUIRE(count > 0);
    platform::File file;
    REQUIRE(file.Open(path, platform::FileOpenMode::CreateNew));
    REQUIRE(file.Close());
    SeenChange seen;
    constexpr uint32 WaitRounds = 50;
    constexpr uint32 WaitMilliseconds = 200;
    for (uint32 round = 0; round < WaitRounds && !seen.added; ++round) {
        (void)source.Wait(WaitMilliseconds);
        REQUIRE(source.Poll(&RecordAdded, &seen));
    }

    CHECK(seen.added);
    source.Close();
}

} // namespace asset_database_lock_tests_file
