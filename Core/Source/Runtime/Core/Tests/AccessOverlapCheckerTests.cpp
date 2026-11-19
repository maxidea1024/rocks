#include <Core/Diagnostics/AccessOverlapChecker.h>
#include <Core/Move.h>
#include <Platform/Process.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;

// 읽기·변경 접근 겹침 검출기 시험이다. 정상 경로(읽기 중첩·여러 스레드의 동시 읽기·재귀 정책)는 이
// 프로세스에서, 위반의 Fatal은 이 실행 파일을 "--process-probe access-overlap <시나리오>"로 다시
// 띄운 자식에서 확인한다(PrepareFatalTestProcess로 오류 대화상자를 막는다). 여러 스레드의 순서는
// 수면이 아닌 명시적 이벤트로 만든다.
namespace rocks::test {
bool RegisterProcessProbe(const char* mode, int (*function)(int, char**) noexcept) noexcept;
} // namespace rocks::test

namespace access_overlap_checker_tests_file {

namespace {

constexpr AccessOverlapMessages ProbeMessages{.writeDuringRead = "probe: write during read",
                                              .readDuringWrite = "probe: read during write",
                                              .writeDuringWrite = "probe: write during write",
                                              .destroyedWhileOpen = "probe: destroyed while open",
                                              .unbalancedRelease = "probe: unbalanced release"};

// 다른 스레드가 읽기 범위를 연 채 신호를 보내고, 놓아 달라는 신호를 기다린다.
struct HeldRead {
    const AccessOverlapChecker* checker = nullptr;
    platform::ManualResetEvent opened;
    platform::ManualResetEvent release;
};

void HoldRead(void* context) noexcept {
    HeldRead& held = *static_cast<HeldRead*>(context);
    ReadAccessScope read(*held.checker);
    held.opened.Signal();
    (void)held.release.Wait();
}

// 자식 쪽이다. Fatal이 나면 돌아오지 않는다. 돌아오면 검출이 빠졌다는 뜻이므로 0을 돌려 부모가
// 실패로 본다.
int AccessOverlapProbe(int argc, char** argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    if (argc < 4) {
        return 20;
    }
    const char* scenario = argv[3];
    if (strcmp(scenario, "write-during-read") == 0) {
        AccessOverlapChecker checker(AccessRecursion::ReadsOnly, ProbeMessages);
        ReadAccessScope read(checker);
        WriteAccessScope write(checker);
        return 0;
    }
    if (strcmp(scenario, "read-during-write") == 0) {
        AccessOverlapChecker checker(AccessRecursion::ReadsOnly, ProbeMessages);
        WriteAccessScope write(checker);
        ReadAccessScope read(checker);
        return 0;
    }
    if (strcmp(scenario, "write-during-write") == 0) {
        // ReadsOnly는 같은 스레드의 변경 재귀도 허용하지 않는다.
        AccessOverlapChecker checker(AccessRecursion::ReadsOnly, ProbeMessages);
        WriteAccessScope outer(checker);
        WriteAccessScope inner(checker);
        return 0;
    }
    if (strcmp(scenario, "read-inside-nested-write") == 0) {
        // NestedWrites는 변경 안의 읽기를 허용하지 않는다.
        AccessOverlapChecker checker(AccessRecursion::NestedWrites, ProbeMessages);
        WriteAccessScope write(checker);
        ReadAccessScope read(checker);
        return 0;
    }
    if (strcmp(scenario, "destroyed-while-open") == 0) {
        alignas(AccessOverlapChecker) unsigned char storage[sizeof(AccessOverlapChecker)];
        AccessOverlapChecker* checker =
            InitializeAt<AccessOverlapChecker>(storage, AccessRecursion::ReadsOnly, ProbeMessages);
        checker->AcquireRead();
        DestroyAt(checker);
        return 0;
    }
    if (strcmp(scenario, "cross-thread-write") == 0) {
        // 다른 스레드가 읽기 범위를 연 것을 이벤트로 확인한 뒤 이 스레드가 변경을 시작한다.
        AccessOverlapChecker checker(AccessRecursion::NestedWritesAndReads, ProbeMessages);
        static HeldRead held;
        held.checker = &checker;
        platform::Thread reader;
        if (reader.Start(&HoldRead, &held) != platform::ThreadStatus::Success) {
            return 21;
        }
        (void)held.opened.Wait();
        WriteAccessScope write(checker);
        return 0;
    }
    if (strcmp(scenario, "unbalanced-release") == 0) {
        const AccessOverlapChecker checker(AccessRecursion::ReadsOnly, ProbeMessages);
        checker.ReleaseRead();
        return 0;
    }
    return 24;
}

const bool AccessOverlapProbeRegistered =
    rocks::test::RegisterProcessProbe("access-overlap", &AccessOverlapProbe);

struct ProbeRun {
    bool ran = false;
    bool abnormal = false;
    char error[4096] = {};
};

// 자식을 띄워 끝날 때까지 기다린다. 기다림에 시간 한도를 두지 않는다(기계 부하로 결과가 바뀌지
// 않게).
ProbeRun RunProbe(const char* scenario) noexcept {
    ProbeRun result;
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return result;
    }
    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"access-overlap", 14}, {scenario, strlen(scenario)}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    platform::ProcessRunOptions options;
    options.error = result.error;
    options.errorCapacity = sizeof(result.error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut) {
        return result;
    }
    result.ran = true;
    result
        .error[run.errorBytes < sizeof(result.error) ? run.errorBytes : sizeof(result.error) - 1] =
        '\0';
    result.abnormal = run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0;
    return result;
}

} // namespace

// 읽기는 같은 스레드에서 겹쳐 열 수 있고(검색 visitor 안 재검색), 모두 닫으면 다시 변경할 수 있다.
TEST(AccessOverlap_ReadsNestAndWritesFollow) {
    AccessOverlapChecker checker;
    CHECK(checker.IsIdle() && checker.Recursion() == AccessRecursion::ReadsOnly);
    {
        ReadAccessScope outer(checker);
        ReadAccessScope inner(checker);
        CHECK(!checker.IsIdle());
    }
    CHECK(checker.IsIdle());
    {
        WriteAccessScope write(checker);
        CHECK(!checker.IsIdle());
    }
    {
        ReadAccessScope read(checker);
    }
    CHECK(checker.IsIdle());
    checker.CheckIdle("not reached");
}

// 두 스레드가 동시에 읽기 범위를 연다(위반 아님). 순서는 이벤트로 만든다: 다른 스레드가 연 것을
// 확인한 뒤 이 스레드가 열고, 닫은 뒤 놓아 준다.
TEST(AccessOverlap_ConcurrentReadersAreAllowed) {
    AccessOverlapChecker checker;
    HeldRead held;
    held.checker = &checker;
    platform::Thread reader;
    REQUIRE(reader.Start(&HoldRead, &held) == platform::ThreadStatus::Success);
    REQUIRE(held.opened.Wait() == platform::WaitResult::Signaled);
    {
        ReadAccessScope read(checker);
        CHECK(!checker.IsIdle());
    }
    held.release.Signal();
    REQUIRE(reader.Join() == platform::ThreadStatus::Success);
    CHECK(checker.IsIdle());
}

// 재귀 정책: NestedWrites는 같은 스레드의 변경 안 변경을, NestedWritesAndReads는 변경 안 읽기까지
// 허용한다. 깊이를 다 풀면 다시 비어 있다.
TEST(AccessOverlap_RecursionPolicies) {
    {
        AccessOverlapChecker checker(AccessRecursion::NestedWrites);
        {
            WriteAccessScope outer(checker);
            WriteAccessScope inner(checker);
            {
                WriteAccessScope deepest(checker);
            }
            CHECK(!checker.IsIdle());
        }
        CHECK(checker.IsIdle());
    }
    {
        AccessOverlapChecker checker(AccessRecursion::NestedWritesAndReads);
        {
            WriteAccessScope write(checker);
            ReadAccessScope read(checker);
            WriteAccessScope nested(checker);
            ReadAccessScope nestedRead(checker);
        }
        CHECK(checker.IsIdle());
        // 변경 밖에서는 평범한 읽기 범위다.
        {
            ReadAccessScope read(checker);
            ReadAccessScope again(checker);
        }
        CHECK(checker.IsIdle());
    }
}

// 위반은 모두 Fatal이고 소유 객체가 준 메시지를 낸다.
TEST(AccessOverlap_ViolationsAreFatal) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }
    REQUIRE(AccessOverlapProbeRegistered);
    struct Case {
        const char* scenario;
        const char* message;
    };
    const Case cases[] = {
        {"write-during-read", "probe: write during read"},
        {"read-during-write", "probe: read during write"},
        {"write-during-write", "probe: write during write"},
        {"read-inside-nested-write", "probe: read during write"},
        {"destroyed-while-open", "probe: destroyed while open"},
        {"cross-thread-write", "probe: write during read"},
        {"unbalanced-release", "probe: unbalanced release"},
    };
    for (const Case& entry : cases) {
        const ProbeRun run = RunProbe(entry.scenario);
        REQUIRE(run.ran);
        CHECK(run.abnormal);
        CHECK(strstr(run.error, entry.message) != nullptr);
    }
}
} // namespace access_overlap_checker_tests_file
