#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Process.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 파일 시스템 에셋 너겟 backend의 실패·경합 시험이다(Docs/Design/AssetSystem 9.1절 결정 8,
// "A3b 구현 세부"): IO 실패 주입(만들기·쓰기·Flush·Close·이동·교체·읽기 열기·읽기·크기·지우기·
// 열거·시각), 같은 프로세스 경합(hook — 같은 키 동시 게시 Exists·Conflict, 판정 전에 지워짐, 잠금
// 안에서 다른 쪽이 고침, GC 대 Put), 두 프로세스 경합(pipe gate), 잡힌 키 잠금, 할당 실패.
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace nugget_backend_race_tests_file {

using rocks::asset_database_test::MemoryFault;
using rocks::asset_database_test::MemoryFileSystem;
using rocks::asset_database_test::MemoryNoReadLimit;
using rocks::asset_database_test::TempProject;

namespace {

constexpr int64 BaseTicks = 639000000000000000ll;
constexpr platform::FileStatus Denied{platform::FileCode::AccessDenied, 5};

class ManualClock final : public NuggetClock {
public:
    int64 NowTicks() noexcept override {
        return now;
    }

    int64 now = BaseTicks;
};

Sha256Digest Key(uint8 seed) noexcept {
    Sha256Digest key;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        key.bytes[index] = static_cast<uint8>(seed * 7 + index);
    }

    return key;
}

Array<uint8> Bytes(Allocator& allocator, usize size, uint32 seed) noexcept {
    Array<uint8> bytes(allocator);
    CHECK(bytes.TryResize(size));
    for (usize index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8>((index / 64 + seed) & 0x0f);
    }

    return bytes;
}

// key의 Content 상대 경로(메모리 파일 시스템 노드 이름)다.
String Relative(Allocator& allocator, const Sha256Digest& key) noexcept {
    char hex[Sha256Digest::HexBytes + 1];
    key.ToHex(hex);
    String path(allocator);
    CHECK(path.TryAppend(StringView(hex, 2)) && path.TryAppend("/") &&
          path.TryAppend(StringView(hex, 64)) && path.TryAppend(".rnug"));
    return path;
}

usize TemporaryCount(MemoryFileSystem& memory) noexcept {
    usize count = 0;
    for (usize index = 0; index < memory.NodeCount(); ++index) {
        const StringView name = memory.NodeName(index);
        count += name.StartsWith("tmp/", StringComparison::CaseSensitive) &&
                         name.EndsWith(".tmp", StringComparison::CaseSensitive)
                     ? 1
                     : 0;
    }

    return count;
}

// 메모리 파일 시스템 장면이다(물리 임시 루트 — 키 잠금).
struct MemoryScene {
    explicit MemoryScene(const char* tag) noexcept
        : project(tag), memory(allocator, project.Root()),
          backend(allocator, memory, project.Root(), clock), payload(Bytes(allocator, 3000, 1)),
          value{{0, payload.View()}} {
        ready = project.made && memory.Ready() && static_cast<bool>(backend.Open());
    }

    NuggetPutResult Put(const Sha256Digest& key) noexcept {
        return backend.Put(NuggetBackendPut{NuggetBucketTexture, key, value});
    }

    NuggetGetResult Get(const Sha256Digest& key) noexcept {
        return backend.Get(NuggetBackendGet{NuggetBucketTexture, key});
    }

    SystemAllocator allocator;
    TempProject project;
    ManualClock clock;
    MemoryFileSystem memory;
    NuggetFileSystemBackend backend;
    Array<uint8> payload;
    NuggetPutValue value[1];
    bool ready = false;
};

} // namespace

// 쓰기 실패: 임시 파일 만들기·n번째 쓰기·Flush·Close·이동(AccessDenied — delete-pending 이름 등)·
// 디렉터리 만들기 실패는 모두 Dropped(그 오류)이고 임시 파일·항목이 남지 않는다.
TEST(NuggetBackendRace_WriteFailures) {
    MemoryScene scene("nugget-write-fail");
    REQUIRE(scene.ready);
    const String relative = Relative(scene.allocator, Key(1));
    auto dropped = [&]() noexcept {
        const NuggetPutResult put = scene.Put(Key(1));
        return put.status == NuggetStatus::Dropped && put.error != ErrorCode::None &&
               TemporaryCount(scene.memory) == 0 && !scene.memory.Has(relative.View());
    };
    scene.memory.faults.create = Denied;
    CHECK(dropped());
    scene.memory.faults.create = {};
    // 쓰기는 머리 + Record 하나·값 하나(표 없음) — 두 번이다.
    for (uint32 call = 1; call <= 2; ++call) {
        scene.memory.faults.failWriteCall = scene.memory.WriteCalls() + call;
        CHECK(dropped());
    }

    scene.memory.faults.failWriteCall = 0;
    scene.memory.faults.flush = Denied;
    CHECK(dropped());
    scene.memory.faults.flush = {};
    scene.memory.faults.close = Denied;
    CHECK(dropped());
    scene.memory.faults.close = {};
    scene.memory.faults.move = Denied;
    CHECK(dropped());
    scene.memory.faults.move = {};
    // 이동 시도가 만든 항목 디렉터리를 지워 디렉터리 만들기 실패를 본다.
    scene.memory.Remove(StringView(relative.View().Data(), 2));
    scene.memory.faults.makeDirectory = Denied;
    CHECK(dropped());
    scene.memory.faults.makeDirectory = {};
    CHECK(scene.Put(Key(1)).status == NuggetStatus::Stored);
}

// 판정 실패: 이동이 AlreadyExists인데 항목이 없고 다시 이동도 실패하면 Dropped(Busy), 기존 항목을
// 읽을 수 없으면(열기 사용 중·크기·읽기 실패) Dropped(그 오류 — 지우지 않는다), 손상 항목 교체
// 실패는 Dropped.
TEST(NuggetBackendRace_SettleFailures) {
    MemoryScene scene("nugget-settle-fail");
    REQUIRE(scene.ready);
    scene.memory.faults.move = platform::FileStatus{platform::FileCode::AlreadyExists, 80};
    const NuggetPutResult ghost = scene.Put(Key(1));
    CHECK(ghost.status == NuggetStatus::Dropped && ghost.error == ErrorCode::Busy &&
          TemporaryCount(scene.memory) == 0);
    scene.memory.faults.move = {};

    REQUIRE(scene.Put(Key(2)).status == NuggetStatus::Stored);
    const String relative = Relative(scene.allocator, Key(2));
    Array<uint8> good(scene.allocator);
    REQUIRE(good.TryAppend(scene.memory.Bytes(relative.View())));
    const MemoryFault faults[] = {MemoryFault::OpenBusy, MemoryFault::Size, MemoryFault::Read};
    for (MemoryFault fault : faults) {
        scene.memory.Remove(relative.View());
        REQUIRE(scene.memory.AddFile(relative.View(), good.View(), fault));
        const NuggetPutResult put = scene.Put(Key(2));
        CHECK(put.status == NuggetStatus::Dropped && put.error != ErrorCode::None &&
              scene.memory.Has(relative.View()) && TemporaryCount(scene.memory) == 0);
        // Get도 그 오류의 Miss다(지우지 않는다).
        NuggetGetResult got = scene.Get(Key(2));
        CHECK(got.status == NuggetStatus::Miss && got.error != ErrorCode::None &&
              scene.memory.Has(relative.View()));
    }

    // 머리·Record는 읽히고 값 byte(파일 끝)만 읽기 실패: 같은 Put의 Exists 확인은 Dropped(그 오류 —
    // 손상으로 보지 않고 지우지 않는다), Get은 Miss(그 오류).
    scene.memory.Remove(relative.View());
    REQUIRE(scene.memory.AddFile(relative.View(), good.View()));
    scene.memory.faults.readFailFrom = good.Size() - 1;
    const NuggetPutResult valueRead = scene.Put(Key(2));
    CHECK(valueRead.status == NuggetStatus::Dropped &&
          valueRead.error == ErrorCode::DependencyFailed && scene.memory.Has(relative.View()) &&
          TemporaryCount(scene.memory) == 0);
    NuggetGetResult valueGet = scene.Get(Key(2));
    CHECK(valueGet.status == NuggetStatus::Miss && valueGet.error == ErrorCode::DependencyFailed &&
          scene.memory.Has(relative.View()));
    scene.memory.faults.readFailFrom = MemoryNoReadLimit;
    CHECK(scene.Put(Key(2)).status == NuggetStatus::Exists);

    // 손상 항목 교체 실패.
    Array<uint8> broken(scene.allocator);
    REQUIRE(broken.TryAppend(good.View()));
    broken[10] ^= 1;
    scene.memory.Remove(relative.View());
    REQUIRE(scene.memory.AddFile(relative.View(), broken.View()));
    scene.memory.faults.replace = Denied;
    const NuggetPutResult replace = scene.Put(Key(2));
    CHECK(replace.status == NuggetStatus::Dropped && TemporaryCount(scene.memory) == 0);
    scene.memory.faults.replace = {};
    // 읽기 열기 실패(접미 .rnug)는 Miss(그 오류)다.
    scene.memory.Remove(relative.View());
    REQUIRE(scene.memory.AddFile(relative.View(), good.View()));
    scene.memory.faults.openRead = Denied;
    scene.memory.faults.readSuffix = ".rnug";
    CHECK(scene.Get(Key(2)).error != ErrorCode::None);
    scene.memory.faults.openRead = {};
    scene.memory.faults.readSuffix = nullptr;
    CHECK(scene.Get(Key(2)).status == NuggetStatus::Hit);
}

// 관리 실패: Remove 지우기 실패는 그 오류, 손상 삭제 실패는 Corrupt 그대로, Touch는 시각을 못
// 바꾸면 그 오류(메모리 FS는 시각을 바꾸지 못한다 — 적중은 무시하고 Hit), GC 하위 디렉터리 열거
// 실패는 건너뜀, 지우기 실패는 skipped, 루트 열거 실패는 오류.
TEST(NuggetBackendRace_ManagementFailures) {
    MemoryScene scene("nugget-manage-fail");
    REQUIRE(scene.ready);
    REQUIRE(scene.Put(Key(1)).status == NuggetStatus::Stored);
    REQUIRE(scene.Put(Key(2)).status == NuggetStatus::Stored);
    CHECK(scene.Get(Key(1)).status == NuggetStatus::Hit);
    CHECK(!scene.backend.Touch(Key(1)));
    scene.memory.faults.remove = Denied;
    CHECK(!scene.backend.Remove(Key(1)));
    // 손상 항목의 삭제 실패.
    const String relative = Relative(scene.allocator, Key(2));
    Array<uint8> broken(scene.allocator);
    REQUIRE(broken.TryAppend(scene.memory.Bytes(relative.View())));
    broken[10] ^= 1;
    scene.memory.Remove(relative.View());
    REQUIRE(scene.memory.AddFile(relative.View(), broken.View()));
    CHECK(scene.Get(Key(2)).status == NuggetStatus::Corrupt && scene.memory.Has(relative.View()));
    // GC: 지우기 실패는 skipped.
    auto collected = scene.backend.Collect(NuggetCollectOptions{0, 0});
    REQUIRE(collected);
    CHECK(collected.Value().removed == 0 && collected.Value().skipped == 2);
    scene.memory.faults.remove = {};
    // 하위 디렉터리 열거 실패는 건너뛴다, 루트 열거 실패는 오류.
    char hex[Sha256Digest::HexBytes + 1];
    Key(1).ToHex(hex);
    const StringView directory(hex, 2);
    scene.memory.Remove(directory);
    REQUIRE(scene.memory.AddDirectory(directory, false, MemoryFault::Enumerate));
    auto skipped = scene.backend.Collect(NuggetCollectOptions{0, 0});
    REQUIRE(skipped);
    CHECK(skipped.Value().skipped >= 1);
    scene.memory.Remove("");
    REQUIRE(scene.memory.AddDirectory("", false, MemoryFault::Enumerate));
    CHECK(!scene.backend.Collect(NuggetCollectOptions{0, 0}));
}

namespace {

// 같은 프로세스 경합 장면이다(실제 디스크 — 두 backend가 같은 루트를 쓴다).
struct RaceScene {
    explicit RaceScene(const char* tag) noexcept
        : project(tag), first(allocator, platform::PhysicalFileSystem(), project.Root(), clock),
          second(allocator, platform::PhysicalFileSystem(), project.Root(), clock),
          a(Bytes(allocator, 2000, 1)), b(Bytes(allocator, 2000, 2)), valueA{{0, a.View()}},
          valueB{{0, b.View()}} {
        ready = project.made && static_cast<bool>(first.Open()) && static_cast<bool>(second.Open());
    }

    SystemAllocator allocator;
    TempProject project;
    ManualClock clock;
    NuggetFileSystemBackend first;
    NuggetFileSystemBackend second;
    Array<uint8> a;
    Array<uint8> b;
    NuggetPutValue valueA[1];
    NuggetPutValue valueB[1];
    bool ready = false;
};

// 한 지점에서 한 번 일을 끼워 넣는 hook이다.
struct Interleave {
    NuggetBackendStep step = NuggetBackendStep::BeforePublish;
    void (*action)(Interleave&) noexcept = nullptr;
    RaceScene* scene = nullptr;
    bool done = false;

    static void Run(void* context, NuggetBackendStep step, const Sha256Digest&) noexcept {
        Interleave& self = *static_cast<Interleave*>(context);
        if (!self.done && step == self.step) {
            self.done = true;
            self.action(self);
        }
    }
};

} // namespace

// 같은 키 동시 게시(이동 직전에 다른 backend가 게시): 같은 Record Exists, 다른 Record Conflict(먼저
// 게시한 쪽이 남는다). 판정 직전에 항목이 지워지면 다시 게시해 Stored. GC가 이동 전에 기존 항목을
// 지우면 그냥 Stored.
TEST(NuggetBackendRace_InProcess) {
    RaceScene scene("nugget-race");
    REQUIRE(scene.ready);
    Interleave same{
        NuggetBackendStep::BeforePublish,
        [](Interleave& self) noexcept {
            CHECK(self.scene->second
                      .Put(NuggetBackendPut{NuggetBucketTexture, Key(1), self.scene->valueA})
                      .status == NuggetStatus::Stored);
        },
        &scene};
    scene.first.SetHooks(NuggetBackendHooks{&Interleave::Run, &same});
    CHECK(scene.first.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), scene.valueA}).status ==
          NuggetStatus::Exists);
    Interleave other{
        NuggetBackendStep::BeforePublish,
        [](Interleave& self) noexcept {
            CHECK(self.scene->second
                      .Put(NuggetBackendPut{NuggetBucketTexture, Key(2), self.scene->valueB})
                      .status == NuggetStatus::Stored);
        },
        &scene};
    scene.first.SetHooks(NuggetBackendHooks{&Interleave::Run, &other});
    const NuggetPutResult conflict =
        scene.first.Put(NuggetBackendPut{NuggetBucketTexture, Key(2), scene.valueA});
    CHECK(conflict.status == NuggetStatus::Conflict);
    NuggetGetResult kept = scene.first.Get(NuggetBackendGet{NuggetBucketTexture, Key(2)});
    CHECK(kept.status == NuggetStatus::Hit && kept.digest == conflict.existing);

    // 판정 직전에 지워짐 → 다시 게시.
    Interleave vanish{
        NuggetBackendStep::BeforeJudge,
        [](Interleave& self) noexcept { CHECK(self.scene->second.Remove(Key(2)).Value()); },
        &scene};
    scene.first.SetHooks(NuggetBackendHooks{&Interleave::Run, &vanish});
    CHECK(scene.first.Put(NuggetBackendPut{NuggetBucketTexture, Key(2), scene.valueA}).status ==
          NuggetStatus::Stored);
    // GC 대 Put: 이동 직전에 GC가 기존 항목을 모두 지운다.
    Interleave collect{NuggetBackendStep::BeforePublish,
                       [](Interleave& self) noexcept {
                           auto done = self.scene->second.Collect(NuggetCollectOptions{0, 0});
                           CHECK(done && done.Value().removed >= 1);
                       },
                       &scene};
    scene.first.SetHooks(NuggetBackendHooks{&Interleave::Run, &collect});
    CHECK(scene.first.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), scene.valueB}).status ==
          NuggetStatus::Stored);
    scene.first.SetHooks({});
}

// 손상 교체의 조건부 갱신: 잠금을 잡고 다시 보기 전에 다른 쪽이 정상 항목으로 고쳤으면 덮어쓰지
// 않는다(같은 Record Exists·다른 Record Conflict).
TEST(NuggetBackendRace_ConditionalReplace) {
    RaceScene scene("nugget-replace-race");
    REQUIRE(scene.ready);
    struct Fix {
        RaceScene* scene;
        String path;
        Array<uint8> broken;
        Array<uint8> good;
    };
    for (uint32 round = 0; round < 2; ++round) {
        REQUIRE(
            scene.second.Put(NuggetBackendPut{NuggetBucketTexture, Key(3), scene.valueA}).status ==
                NuggetStatus::Stored ||
            round > 0);
        char hex[Sha256Digest::HexBytes + 1];
        Key(3).ToHex(hex);
        String path(scene.allocator);
        REQUIRE(path.TryAppend(scene.project.Root()) && path.TryAppend("/") &&
                path.TryAppend(StringView(hex, 2)) && path.TryAppend("/") &&
                path.TryAppend(StringView(hex, 64)) && path.TryAppend(".rnug"));
        // 정상 byte를 기억하고 손상시킨다.
        Array<uint8> good(scene.allocator);
        {
            platform::File file;
            uint64 size = 0;
            REQUIRE(file.Open(platform::PhysicalFileSystem(), path.View().Data(),
                              path.View().ByteCount(), platform::FileOpenMode::ReadExisting) &&
                    file.Size(size) && good.TryResize(static_cast<usize>(size)) &&
                    file.ReadExact(good.Data(), good.Size()).status);
            (void)file.Close();
        }

        Array<uint8> broken(scene.allocator);
        REQUIRE(broken.TryAppend(good.View()));
        broken[12] ^= 1;
        auto write = [&](Span<const uint8> bytes) noexcept {
            platform::File file;
            const bool ok = static_cast<bool>(file.Open(platform::PhysicalFileSystem(),
                                                        path.View().Data(), path.View().ByteCount(),
                                                        platform::FileOpenMode::CreateAlways)) &&
                            static_cast<bool>(file.WriteAll(bytes.Data(), bytes.Size()).status);
            return static_cast<bool>(file.Close()) && ok;
        };
        REQUIRE(write(broken.View()));
        static Fix* fix = nullptr;
        Fix state{&scene, Move(path), Move(broken), Move(good)};
        fix = &state;
        Interleave repair{NuggetBackendStep::BeforeJudgeLocked,
                          [](Interleave&) noexcept {
                              platform::File file;
                              CHECK(file.Open(platform::PhysicalFileSystem(),
                                              fix->path.View().Data(), fix->path.View().ByteCount(),
                                              platform::FileOpenMode::CreateAlways) &&
                                    file.WriteAll(fix->good.Data(), fix->good.Size()).status);
                              CHECK(file.Close());
                          },
                          &scene};
        scene.first.SetHooks(NuggetBackendHooks{&Interleave::Run, &repair});
        const NuggetPutResult put = scene.first.Put(NuggetBackendPut{
            NuggetBucketTexture, Key(3), round == 0 ? scene.valueA : scene.valueB});
        CHECK(put.status == (round == 0 ? NuggetStatus::Exists : NuggetStatus::Conflict));
        scene.first.SetHooks({});
        fix = nullptr;
    }
}

namespace {

constexpr int ProbeUsageExit = 2;
constexpr int ProbeFailureExit = 4;
// 자식 Put 결과의 종료 코드다(ProbeStatusBase + NuggetStatus).
constexpr int ProbeStatusBase = 20;
constexpr char ReadySignal[] = "ready";
constexpr char HeldSignal[] = "held";
// 다른 프로세스가 shard 잠금을 쥔 시험의 대기 상한(ms)이다(기본 5초를 기다리지 않는다).
constexpr uint32 HeldLockTimeoutMs = 50;

bool Signal(const char* text, usize bytes) noexcept {
    return fwrite(text, 1, bytes, stdout) == bytes && fflush(stdout) == 0;
}

// 자식: argv[3] 루트에 argv[4] seed 값으로 Key(9)를 Put한다. 이동 직전에 "ready"를 쓰고 표준 입력
// 1 byte를 기다린다.
int PutProbe(int argc, const char* const* argv) noexcept {
    constexpr int RootArgument = 3;
    constexpr int SeedArgument = 4;
    if (argc <= SeedArgument) {
        return ProbeUsageExit;
    }

    SystemAllocator allocator;
    ManualClock clock;
    NuggetFileSystemBackend backend(allocator, platform::PhysicalFileSystem(),
                                    StringView(argv[RootArgument], strlen(argv[RootArgument])),
                                    clock);
    if (!backend.Open()) {
        return ProbeFailureExit;
    }

    struct Gate {
        static void Run(void*, NuggetBackendStep step, const Sha256Digest&) noexcept {
            if (step == NuggetBackendStep::BeforePublish) {
                (void)Signal(ReadySignal, sizeof(ReadySignal) - 1);
                char gate = 0;
                (void)fread(&gate, 1, 1, stdin);
            }
        }
    };
    backend.SetHooks(NuggetBackendHooks{&Gate::Run, nullptr});
    const Array<uint8> value =
        Bytes(allocator, 2000, static_cast<uint32>(argv[SeedArgument][0] - '0'));
    const NuggetPutValue values[] = {{0, value.View()}};
    const NuggetPutResult put = backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(9), values});
    return ProbeStatusBase + static_cast<int>(put.status);
}

// 자식: argv[3] 경로의 키 잠금을 잡고 "held"를 쓴 뒤 표준 입력 1 byte를 기다린다.
int LockProbe(int argc, const char* const* argv) noexcept {
    constexpr int PathArgument = 3;
    if (argc <= PathArgument) {
        return ProbeUsageExit;
    }

    platform::FileLock lock;
    if (!lock.Acquire(argv[PathArgument], 0) || !Signal(HeldSignal, sizeof(HeldSignal) - 1)) {
        return ProbeFailureExit;
    }

    char gate = 0;
    (void)fread(&gate, 1, 1, stdin);
    lock.Release();
    return 0;
}

const bool PutProbeRegistered = rocks::test::RegisterProcessProbe("nugget-put", &PutProbe);
const bool LockProbeRegistered = rocks::test::RegisterProcessProbe("nugget-lock", &LockProbe);

bool StartChild(platform::Process& child, StringView mode, StringView first,
                StringView second) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    const platform::ProcessArgument arguments[] = {{"--process-probe", 15},
                                                   {mode.Data(), mode.ByteCount()},
                                                   {first.Data(), first.ByteCount()},
                                                   {second.Data(), second.ByteCount()}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = second.IsEmpty() ? 3 : 4;
    info.input = platform::ProcessStreamMode::Pipe;
    info.output = platform::ProcessStreamMode::Pipe;
    info.error = platform::ProcessStreamMode::Null;
    return static_cast<bool>(child.Start(info));
}

bool WaitSignal(platform::Process& child, const char* signal, usize bytes) noexcept {
    char buffer[16] = {};
    usize received = 0;
    while (received < bytes) {
        const platform::FileTransfer read = child.Read(
            platform::ProcessStream::Output, buffer + received, sizeof(buffer) - 1 - received);
        if (!read.status || read.bytes == 0) {
            return false;
        }

        received += read.bytes;
    }

    return strncmp(buffer, signal, bytes) == 0;
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

// 두 프로세스 같은 키 동시 게시(9.1절 결정 8): 자식이 임시 파일을 다 쓰고 이동 직전에 멈춘 사이
// 부모가 게시하면, 자식은 같은 값이면 Exists·다른 값이면 Conflict다(pipe gate — 시간에 기대지
// 않는다).
TEST(NuggetBackendRace_TwoProcesses) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }

    REQUIRE(PutProbeRegistered);
    SystemAllocator allocator;
    TempProject project("nugget-two-process");
    REQUIRE(project.made);
    ManualClock clock;
    NuggetFileSystemBackend backend(allocator, platform::PhysicalFileSystem(), project.Root(),
                                    clock);
    REQUIRE(backend.Open());
    const char* seeds[] = {"1", "2"};
    const NuggetStatus expected[] = {NuggetStatus::Exists, NuggetStatus::Conflict};
    for (usize round = 0; round < 2; ++round) {
        (void)backend.Remove(Key(9));
        platform::Process child;
        REQUIRE(StartChild(child, "nugget-put", project.Root(), StringView(seeds[round], 1)));
        REQUIRE(WaitSignal(child, ReadySignal, sizeof(ReadySignal) - 1));
        const Array<uint8> value = Bytes(allocator, 2000, 1);
        const NuggetPutValue values[] = {{0, value.View()}};
        CHECK(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(9), values}).status ==
              NuggetStatus::Stored);
        const char gate = 'g';
        REQUIRE(child.WriteInput(&gate, 1).status);
        CHECK(ExitCode(child) == ProbeStatusBase + static_cast<int>(expected[round]));
    }
}

// 다른 프로세스가 shard 잠금(`tmp/<k0k1>.lock`)을 쥐고 있으면 손상 항목 교체는 주입한 잠금 대기
// 상한 뒤 Dropped(Busy)이고 항목은 그대로다. 풀린 뒤에는 교체하고 잠금 파일은 남는다.
TEST(NuggetBackendRace_HeldShardLock) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }

    REQUIRE(LockProbeRegistered);
    SystemAllocator allocator;
    TempProject project("nugget-held-lock");
    REQUIRE(project.made);
    ManualClock clock;
    NuggetFileSystemBackend backend(allocator, platform::PhysicalFileSystem(), project.Root(),
                                    clock);
    REQUIRE(backend.Open());
    backend.SetReplaceLockTimeout(HeldLockTimeoutMs);
    const Array<uint8> value = Bytes(allocator, 2000, 1);
    const NuggetPutValue values[] = {{0, value.View()}};
    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(4), values}).status ==
            NuggetStatus::Stored);
    char hex[Sha256Digest::HexBytes + 1];
    Key(4).ToHex(hex);
    String entry(allocator);
    String lock(allocator);
    REQUIRE(entry.TryAppend(project.Root()) && entry.TryAppend("/") &&
            entry.TryAppend(StringView(hex, 2)) && entry.TryAppend("/") &&
            entry.TryAppend(StringView(hex, 64)) && entry.TryAppend(".rnug") &&
            lock.TryAppend(project.Root()) && lock.TryAppend("/tmp/") &&
            lock.TryAppend(StringView(hex, 2)) && lock.TryAppend(".lock"));
    {
        platform::File file;
        REQUIRE(file.Open(platform::PhysicalFileSystem(), entry.View().Data(),
                          entry.View().ByteCount(), platform::FileOpenMode::ReadWriteExisting));
        const uint8 junk = 0xee;
        uint64 position = 0;
        REQUIRE(file.Seek(9, platform::FileSeekOrigin::Begin, position) &&
                file.WriteAll(&junk, 1).status);
        REQUIRE(file.Close());
    }

    platform::Process child;
    REQUIRE(StartChild(child, "nugget-lock", lock.View(), StringView()));
    REQUIRE(WaitSignal(child, HeldSignal, sizeof(HeldSignal) - 1));
    const NuggetPutResult put = backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(4), values});
    CHECK(put.status == NuggetStatus::Dropped && put.error == ErrorCode::Busy);
    const char gate = 'g';
    REQUIRE(child.WriteInput(&gate, 1).status);
    CHECK(ExitCode(child) == 0);
    // 잠금이 풀린 뒤에는 교체한다.
    CHECK(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(4), values}).status ==
          NuggetStatus::Stored);
    bool lockKept = false;
    CHECK(platform::PhysicalFileSystem().FileExists(lock.View().Data(), lock.View().ByteCount(),
                                                    lockKept) &&
          lockKept);
}

// 할당 실패(Put·Get·Collect): Put은 Dropped(OutOfMemory)이고 임시 파일·반쪽 항목이 남지 않는다,
// Get은 Miss(OutOfMemory)이고 정상 항목을 지우지 않는다, Collect는 OutOfMemory거나 성공이다. 실패
// 없는 결과는 기준과 같다.
TEST(NuggetBackendRace_AllocationFailures) {
    SystemAllocator system;
    TempProject project("nugget-oom");
    REQUIRE(project.made);
    ManualClock clock;
    const Array<uint8> small = Bytes(system, 300, 1);
    const Array<uint8> packed = Bytes(system, 200000, 2);
    const NuggetPutValue values[] = {{0, small.View()}, {1, packed.View()}};
    test::NFailAllocator failing(system);
    NuggetFileSystemBackend backend(failing, platform::PhysicalFileSystem(), project.Root(), clock);
    REQUIRE(backend.Open());
    char hex[Sha256Digest::HexBytes + 1];
    Key(1).ToHex(hex);
    String entry(system);
    REQUIRE(entry.TryAppend(project.Root()) && entry.TryAppend("/") &&
            entry.TryAppend(StringView(hex, 2)) && entry.TryAppend("/") &&
            entry.TryAppend(StringView(hex, 64)) && entry.TryAppend(".rnug"));
    auto exists = [&]() noexcept {
        bool found = false;
        return platform::PhysicalFileSystem().FileExists(entry.View().Data(),
                                                         entry.View().ByteCount(), found) &&
               found;
    };
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        const NuggetPutResult put =
            backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(put.status == NuggetStatus::Stored && exists());
            break;
        }

        CHECK(put.status == NuggetStatus::Dropped && put.error == ErrorCode::OutOfMemory &&
              !exists());
        ++failures;
    }

    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        NuggetGetResult got = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(got.status == NuggetStatus::Hit && got.values.Size() == 2);
            CHECK(got.values[1].bytes.Size() == packed.Size());
            break;
        }

        CHECK(got.status == NuggetStatus::Miss && got.error == ErrorCode::OutOfMemory && exists());
        ++failures;
    }

    // 같은 Put(판정 경로)의 할당 실패.
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        const NuggetPutResult put =
            backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(put.status == NuggetStatus::Exists);
            break;
        }

        CHECK(put.status == NuggetStatus::Dropped && put.error == ErrorCode::OutOfMemory &&
              exists());
        ++failures;
    }

    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto collected = backend.Collect(NuggetCollectOptions{~uint64{0}, 3650});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(collected);
            CHECK(collected.Value().kept == 1 && exists());
            break;
        }

        CHECK(collected.Error() == ErrorCode::OutOfMemory && exists());
        ++failures;
    }

    CHECK(failures > 10);
}

// 할당 실패(열기·값 선택 partial Get·손상 항목 교체 Put): 모두 OutOfMemory(열기는 오류, Get은
// Miss, Put은 Dropped)이고 손상이 아닌 항목을 지우지 않으며, 실패 없는 결과는 기준과 같다. tmp 열거
// 실패는 열기·GC가 무시한다(정리만 건너뛴다).
TEST(NuggetBackendRace_MoreAllocationFailures) {
    SystemAllocator system;
    TempProject project("nugget-oom-more");
    REQUIRE(project.made);
    ManualClock clock;
    test::NFailAllocator failing(system);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        NuggetFileSystemBackend backend(failing, platform::PhysicalFileSystem(), project.Root(),
                                        clock);
        failing.FailOnAttempt(attempt);
        auto opened = backend.Open();
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(opened);
            break;
        }

        CHECK(opened.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    NuggetFileSystemBackend backend(failing, platform::PhysicalFileSystem(), project.Root(), clock);
    REQUIRE(backend.Open());
    const Array<uint8> a = Bytes(system, 300, 1);
    const Array<uint8> b = Bytes(system, 300, 2);
    const NuggetPutValue values[] = {{0, a.View()}, {2, b.View()}};
    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values}).status ==
            NuggetStatus::Stored);
    const uint32 chosen[] = {2, 5};
    for (uint64 attempt = 1;; ++attempt) {
        NuggetBackendGet get{NuggetBucketTexture, Key(1), chosen};
        get.partial = true;
        failing.FailOnAttempt(attempt);
        NuggetGetResult got = backend.Get(get);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(got.status == NuggetStatus::Partial && got.values.Size() == 1);
            CHECK(got.values[0].id == 2);
            break;
        }

        CHECK(got.status == NuggetStatus::Miss && got.error == ErrorCode::OutOfMemory);
        ++failures;
    }

    // 손상 항목 교체 경로: 시도마다 손상 항목을 다시 둔다.
    char hex[Sha256Digest::HexBytes + 1];
    Key(1).ToHex(hex);
    String entry(system);
    REQUIRE(entry.TryAppend(project.Root()) && entry.TryAppend("/") &&
            entry.TryAppend(StringView(hex, 2)) && entry.TryAppend("/") &&
            entry.TryAppend(StringView(hex, 64)) && entry.TryAppend(".rnug"));
    Array<uint8> good(system);
    {
        platform::File file;
        uint64 size = 0;
        REQUIRE(file.Open(platform::PhysicalFileSystem(), entry.View().Data(),
                          entry.View().ByteCount(), platform::FileOpenMode::ReadExisting) &&
                file.Size(size) && good.TryResize(static_cast<usize>(size)) &&
                file.ReadExact(good.Data(), good.Size()).status);
        (void)file.Close();
    }

    auto corrupt = [&]() noexcept {
        Array<uint8> broken(system);
        CHECK(broken.TryAppend(good.View()));
        broken[12] ^= 1;
        platform::File file;
        CHECK(file.Open(platform::PhysicalFileSystem(), entry.View().Data(),
                        entry.View().ByteCount(), platform::FileOpenMode::CreateAlways) &&
              file.WriteAll(broken.Data(), broken.Size()).status);
        CHECK(file.Close());
    };
    for (uint64 attempt = 1;; ++attempt) {
        corrupt();
        failing.FailOnAttempt(attempt);
        const NuggetPutResult put =
            backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(put.status == NuggetStatus::Stored);
            break;
        }

        CHECK(put.status == NuggetStatus::Dropped && put.error == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 5);
}

// tmp 열거 실패는 열기·GC가 무시한다(정리만 건너뛴다).
TEST(NuggetBackendRace_TemporaryListingFailure) {
    MemoryScene scene("nugget-tmp-list");
    REQUIRE(scene.ready);
    scene.memory.Remove("tmp");
    REQUIRE(scene.memory.AddDirectory("tmp", false, MemoryFault::Enumerate));
    NuggetFileSystemBackend again(scene.allocator, scene.memory, scene.project.Root(), scene.clock);
    REQUIRE(again.Open());
    auto collected = again.Collect(NuggetCollectOptions{~uint64{0}, 365});
    REQUIRE(collected);
    CHECK(collected.Value().temporaries == 0);
}

// GC 이름 거름(항목 디렉터리 안): 하위 디렉터리, 길이가 다른 이름, 다른 확장자, 16진이 아닌 키,
// 디렉터리와 접두가 다른 키는 건드리지 않는다. Touch·Remove·GC(하위 디렉터리·지난 임시 파일 있음)의
// 할당 실패는 OutOfMemory이고 항목이 남는다.
TEST(NuggetBackendRace_CollectFilterAndAllocations) {
    SystemAllocator system;
    TempProject project("nugget-collect-filter");
    REQUIRE(project.made);
    ManualClock clock;
    test::NFailAllocator failing(system);
    NuggetFileSystemBackend backend(failing, platform::PhysicalFileSystem(), project.Root(), clock);
    REQUIRE(backend.Open());
    const Array<uint8> payload = Bytes(system, 500, 1);
    const NuggetPutValue values[] = {{0, payload.View()}};
    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values}).status ==
            NuggetStatus::Stored);
    char hex[Sha256Digest::HexBytes + 1];
    Key(1).ToHex(hex);
    String directory(system);
    REQUIRE(directory.TryAppend(project.Root()) && directory.TryAppend("/") &&
            directory.TryAppend(StringView(hex, 2)));
    platform::FileSystem& files = platform::PhysicalFileSystem();
    auto write = [&](StringView name) noexcept {
        String path(system);
        CHECK(path.TryAppend(directory.View()) && path.TryAppend("/") && path.TryAppend(name));
        platform::File file;
        CHECK(file.Open(files, path.View().Data(), path.View().ByteCount(),
                        platform::FileOpenMode::CreateAlways) &&
              file.WriteAll(payload.Data(), payload.Size()).status);
        CHECK(file.Close());
        return path;
    };
    // 64자 이름 변형들.
    char other[Sha256Digest::HexBytes + 1];
    Key(2).ToHex(other);
    String wrongPrefix(system);
    REQUIRE(wrongPrefix.TryAppend(StringView(other, 64)) && wrongPrefix.TryAppend(".rnug"));
    String nonHex(system);
    REQUIRE(nonHex.TryAppend(StringView(hex, 2)) && nonHex.TryAppend(StringView(hex + 2, 61)) &&
            nonHex.TryAppend("Z.rnug"));
    String wrongExtension(system);
    REQUIRE(wrongExtension.TryAppend(StringView(hex, 2)) &&
            wrongExtension.TryAppend(StringView(hex + 2, 62)) && wrongExtension.TryAppend(".rnux"));
    const String strays[] = {write(wrongPrefix.View()), write(nonHex.View()),
                             write(wrongExtension.View()), write("short.rnug")};
    String nested(system);
    REQUIRE(nested.TryAppend(directory.View()) && nested.TryAppend("/nested"));
    REQUIRE(files.MakeDirectory(nested.View().Data(), nested.View().ByteCount()));
    // 지난 임시 파일(GC 정리 경로의 할당).
    String stale(system);
    REQUIRE(stale.TryAppend(project.Root()) && stale.TryAppend("/tmp/stale.tmp"));
    auto addStale = [&]() noexcept {
        platform::File file;
        CHECK(file.Open(files, stale.View().Data(), stale.View().ByteCount(),
                        platform::FileOpenMode::CreateAlways) &&
              file.WriteAll(payload.Data(), payload.Size()).status);
        CHECK(file.Close());
        CHECK(files.SetModifiedTime(stale.View().Data(), stale.View().ByteCount(),
                                    clock.now - NuggetTemporaryLifetimeTicks - 1));
    };

    addStale();
    auto collected = backend.Collect(NuggetCollectOptions{0, 0});
    REQUIRE(collected);
    CHECK(collected.Value().removed == 1 && collected.Value().temporaries == 1);
    for (const String& path : strays) {
        bool found = false;
        CHECK(files.FileExists(path.View().Data(), path.View().ByteCount(), found) && found);
    }

    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values}).status ==
            NuggetStatus::Stored);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto touched = backend.Touch(Key(1));
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(touched);
            break;
        }

        CHECK(touched.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    for (uint64 attempt = 1;; ++attempt) {
        addStale();
        failing.FailOnAttempt(attempt);
        auto done = backend.Collect(NuggetCollectOptions{~uint64{0}, 3650});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(done);
            CHECK(done.Value().kept == 1 && done.Value().temporaries == 1);
            break;
        }

        CHECK(done.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto removed = backend.Remove(Key(1));
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(removed && removed.Value());
            break;
        }

        CHECK(removed.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 3);
}

// 메모리 파일 시스템 경계: GC는 link인 항목 디렉터리·link인 항목 파일을 건드리지 않는다, 지난 임시
// 파일을 지우지 못하면 세지 않는다, 손상 항목 교체가 NotDurable이면(교체는 보인다) Stored다.
TEST(NuggetBackendRace_LinksAndDurability) {
    MemoryScene scene("nugget-links");
    REQUIRE(scene.ready);
    REQUIRE(scene.Put(Key(1)).status == NuggetStatus::Stored);
    const String relative = Relative(scene.allocator, Key(1));
    // link인 디렉터리(16진 두 글자)와 항목 디렉터리 안 link인 항목 파일.
    REQUIRE(scene.memory.AddDirectory("ab", true));
    char hex[Sha256Digest::HexBytes + 1];
    Key(2).ToHex(hex);
    String linked(scene.allocator);
    REQUIRE(linked.TryAppend(StringView(relative.View().Data(), 3)) &&
            linked.TryAppend(StringView(relative.View().Data(), 2)) &&
            linked.TryAppend(StringView(hex + 2, 62)) && linked.TryAppend(".rnug"));
    REQUIRE(scene.memory.AddFile(linked.View(), scene.payload.View(), MemoryFault::None,
                                 platform::FileType::File, true));
    // 지울 수 없는 지난 임시 파일(메모리 파일 시스템의 시각은 아주 옛날이다).
    REQUIRE(scene.memory.AddFile("tmp/old.tmp", scene.payload.View()));
    scene.memory.faults.remove = Denied;
    scene.memory.faults.statPath = nullptr;
    auto blocked = scene.backend.Collect(NuggetCollectOptions{~uint64{0}, 365000});
    REQUIRE(blocked);
    CHECK(blocked.Value().temporaries == 0 && blocked.Value().kept == 1 &&
          scene.memory.Has("tmp/old.tmp") && scene.memory.Has(linked.View()));
    scene.memory.faults.remove = {};

    // 손상 항목 교체가 NotDurable이면 Stored다.
    Array<uint8> broken(scene.allocator);
    REQUIRE(broken.TryAppend(scene.memory.Bytes(relative.View())));
    broken[12] ^= 1;
    scene.memory.Remove(relative.View());
    REQUIRE(scene.memory.AddFile(relative.View(), broken.View()));
    scene.memory.faults.replace = platform::FileStatus{platform::FileCode::NotDurable, 0};
    CHECK(scene.Put(Key(1)).status == NuggetStatus::Stored);
    scene.memory.faults.replace = {};
    CHECK(scene.Get(Key(1)).status == NuggetStatus::Hit);
}

} // namespace nugget_backend_race_tests_file
