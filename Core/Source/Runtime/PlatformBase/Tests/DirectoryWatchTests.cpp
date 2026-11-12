#include "../Private/Common/DirectoryWatchInternal.h"

#include <Platform/Clock.h>
#include <Platform/DirectoryWatch.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>
#include <Platform/Atomics.h>

using namespace rocks;
using namespace rocks::platform;

// 실제 OS 파일 시스템에서 DirectoryWatcher의 이식 계약(14절 시나리오 17)을 검사한다. 이벤트
// 개수·종류 세부는 플랫폼마다 다르므로 "경로에 대한 이벤트가 결국 온다"와 허용된 종류만 확인한다.
namespace {

struct Scratch {
    char path[200]{};
    usize length = 0;

    bool Make(const char* tag) noexcept {
        static int counter = 0;
        const int count = snprintf(path, sizeof(path), "rocks-watch-%s-%llu-%d", tag,
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
    return written.status && static_cast<bool>(file.Close());
}

struct Event {
    DirectoryChangeKind kind;
    char path[128];
    char oldPath[128];
};

struct Recorder {
    Event events[256];
    int count = 0;
    int acceptBeforeStop = 1 << 30;

    static bool Visit(const DirectoryChange& change, void* context) noexcept {
        auto& self = *static_cast<Recorder*>(context);
        if (self.acceptBeforeStop <= 0) {
            return false;
        }
        --self.acceptBeforeStop;
        if (self.count < 256) {
            Event& event = self.events[self.count++];
            event.kind = change.kind;
            snprintf(event.path, sizeof(event.path), "%s", change.path);
            snprintf(event.oldPath, sizeof(event.oldPath), "%s", change.oldPath);
        }
        return true;
    }

    bool Has(DirectoryChangeKind kind, const char* path) const noexcept {
        for (int index = 0; index < count; ++index) {
            if (events[index].kind == kind && strcmp(events[index].path, path) == 0) {
                return true;
            }
        }
        return false;
    }

    bool HasAny(const char* path) const noexcept {
        for (int index = 0; index < count; ++index) {
            if (strcmp(events[index].path, path) == 0 || strcmp(events[index].oldPath, path) == 0) {
                return true;
            }
        }
        return false;
    }

    bool HasKind(DirectoryChangeKind kind) const noexcept {
        for (int index = 0; index < count; ++index) {
            if (events[index].kind == kind) {
                return true;
            }
        }
        return false;
    }
};

// predicate가 참이 될 때까지(최대 5초) 기다리며 이벤트를 모은다.
template <class Predicate>
bool PollUntil(DirectoryWatcher& watcher, Recorder& recorder, Predicate&& done) noexcept {
    const uint64 deadline = MonotonicNanoseconds() + 5000000000ull;
    while (MonotonicNanoseconds() < deadline) {
        (void)watcher.Wait(50);
        if (!watcher.Poll(&Recorder::Visit, &recorder)) {
            return false;
        }
        if (done()) {
            return true;
        }
    }
    return done();
}

} // namespace

TEST(DirectoryWatch_OpenRejectsMissingFileAndDoubleOpen) {
    Scratch scratch;
    REQUIRE(scratch.Make("open"));
    DirectoryWatcher watcher;
    Recorder recorder;
    CHECK(watcher.Poll(&Recorder::Visit, &recorder).code == FileCode::NotOpen);
    CHECK(!watcher.Wait(0));

    char missing[256];
    CHECK(watcher.Open(scratch.Child("missing", missing, sizeof(missing))).code ==
          FileCode::NotFound);
    char file[256];
    REQUIRE(WriteFile(scratch.Child("file.txt", file, sizeof(file)), "x"));
    CHECK(watcher.Open(file).code == FileCode::WrongType);
    CHECK(!watcher.IsOpen());

    REQUIRE(watcher.Open(scratch.path, scratch.length));
    CHECK(watcher.Open(scratch.path, scratch.length).code == FileCode::AlreadyOpen);
    watcher.Close();
    watcher.Close();
    CHECK(!watcher.IsOpen());
}

TEST(DirectoryWatch_ReportsCreateModifyRenameAndDelete) {
    Scratch scratch;
    REQUIRE(scratch.Make("events"));
    DirectoryWatcher watcher;
    REQUIRE(watcher.Open(scratch.path, scratch.length));
    Recorder recorder;

    char a[256];
    REQUIRE(WriteFile(scratch.Child("a.txt", a, sizeof(a)), "first"));
    CHECK(PollUntil(watcher, recorder, [&] { return recorder.HasAny("a.txt"); }));

    recorder.count = 0;
    REQUIRE(WriteFile(a, "second write"));
    CHECK(PollUntil(watcher, recorder, [&] {
        return recorder.Has(DirectoryChangeKind::Modified, "a.txt") ||
               recorder.Has(DirectoryChangeKind::Added, "a.txt");
    }));

    // 이름 변경은 Renamed(b ← a) 또는 Removed(a) + Added(b)다.
    recorder.count = 0;
    char b[256];
    scratch.Child("b.txt", b, sizeof(b));
    REQUIRE(PhysicalFileSystem().Move(a, strlen(a), b, strlen(b), false));
    CHECK(PollUntil(watcher, recorder, [&] {
        bool renamed = false;
        for (int index = 0; index < recorder.count; ++index) {
            const Event& event = recorder.events[index];
            renamed = renamed ||
                      (event.kind == DirectoryChangeKind::Renamed &&
                       strcmp(event.path, "b.txt") == 0 && strcmp(event.oldPath, "a.txt") == 0);
        }
        return renamed || (recorder.Has(DirectoryChangeKind::Removed, "a.txt") &&
                           recorder.Has(DirectoryChangeKind::Added, "b.txt"));
    }));

    recorder.count = 0;
    REQUIRE(PhysicalFileSystem().Delete(b, strlen(b)));
    CHECK(PollUntil(watcher, recorder,
                    [&] { return recorder.Has(DirectoryChangeKind::Removed, "b.txt"); }));
    CHECK(!recorder.HasKind(DirectoryChangeKind::RootRemoved));
}

TEST(DirectoryWatch_ReportsChangesInsideNewSubdirectories) {
    Scratch scratch;
    REQUIRE(scratch.Make("subdir"));
    DirectoryWatcher watcher;
    REQUIRE(watcher.Open(scratch.path, scratch.length));
    Recorder recorder;

    char sub[256];
    scratch.Child("sub", sub, sizeof(sub));
    REQUIRE(PhysicalFileSystem().MakeDirectory(sub, strlen(sub)));
    char nested[256];
    scratch.Child("sub/deep", nested, sizeof(nested));
    REQUIRE(PhysicalFileSystem().MakeDirectory(nested, strlen(nested)));
    char file[256];
    REQUIRE(WriteFile(scratch.Child("sub/deep/c.txt", file, sizeof(file)), "nested"));
    // 디렉터리를 만든 직후 생긴 파일도 놓치지 않는다(inotify watch 경합 보정 포함).
    CHECK(PollUntil(watcher, recorder, [&] { return recorder.HasAny("sub/deep/c.txt"); }));

    recorder.count = 0;
    REQUIRE(WriteFile(file, "nested again"));
    CHECK(PollUntil(watcher, recorder, [&] { return recorder.HasAny("sub/deep/c.txt"); }));
}

TEST(DirectoryWatch_VisitorCanDeferAndReentryIsBusy) {
    Scratch scratch;
    REQUIRE(scratch.Make("defer"));
    DirectoryWatcher watcher;
    REQUIRE(watcher.Open(scratch.path, scratch.length));
    char first[256];
    char second[256];
    REQUIRE(WriteFile(scratch.Child("one.txt", first, sizeof(first)), "1"));
    REQUIRE(WriteFile(scratch.Child("two.txt", second, sizeof(second)), "2"));

    // visitor가 거절한 이벤트는 버리지 않고 다음 Poll의 첫 이벤트로 다시 온다.
    struct Refuser {
        Event refused{};
        bool seen = false;
    } refuser;
    const uint64 deadline = MonotonicNanoseconds() + 5000000000ull;
    while (!refuser.seen && MonotonicNanoseconds() < deadline) {
        (void)watcher.Wait(50);
        REQUIRE(watcher.Poll(
            [](const DirectoryChange& change, void* context) noexcept {
                auto& self = *static_cast<Refuser*>(context);
                self.refused.kind = change.kind;
                snprintf(self.refused.path, sizeof(self.refused.path), "%s", change.path);
                self.seen = true;
                return false;
            },
            &refuser));
    }
    REQUIRE(refuser.seen);
    Recorder next;
    REQUIRE(watcher.Poll(&Recorder::Visit, &next));
    REQUIRE(next.count >= 1);
    CHECK(next.events[0].kind == refuser.refused.kind &&
          strcmp(next.events[0].path, refuser.refused.path) == 0);

    struct Reentry {
        DirectoryWatcher* watcher;
        FileCode inner = FileCode::Success;
        bool called = false;
    } reentry{&watcher};
    REQUIRE(WriteFile(first, "again"));
    const uint64 reentryDeadline = MonotonicNanoseconds() + 5000000000ull;
    while (!reentry.called && MonotonicNanoseconds() < reentryDeadline) {
        (void)watcher.Wait(50);
        REQUIRE(watcher.Poll(
            [](const DirectoryChange&, void* context) noexcept {
                auto& self = *static_cast<Reentry*>(context);
                Recorder ignored;
                self.inner = self.watcher->Poll(&Recorder::Visit, &ignored).code;
                self.called = true;
                return true;
            },
            &reentry));
    }
    CHECK(reentry.called);
    CHECK(reentry.inner == FileCode::Busy);
}

TEST(DirectoryWatch_CloseInsideVisitorIsDeferred) {
    Scratch scratch;
    REQUIRE(scratch.Make("close"));
    DirectoryWatcher watcher;
    REQUIRE(watcher.Open(scratch.path, scratch.length));
    char file[256];
    REQUIRE(WriteFile(scratch.Child("a.txt", file, sizeof(file)), "1"));
    REQUIRE(WriteFile(scratch.Child("b.txt", file, sizeof(file)), "2"));
    struct Closer {
        DirectoryWatcher* watcher;
        int calls = 0;
    } closer{&watcher};
    const uint64 deadline = MonotonicNanoseconds() + 5000000000ull;
    while (closer.calls == 0 && watcher.IsOpen() && MonotonicNanoseconds() < deadline) {
        (void)watcher.Wait(50);
        (void)watcher.Poll(
            [](const DirectoryChange&, void* context) noexcept {
                auto& self = *static_cast<Closer*>(context);
                ++self.calls;
                self.watcher->Close(); // RootRemoved를 받으면 닫는 소비자 흐름
                return true;
            },
            &closer);
    }
    // Close는 visitor가 돌아온 뒤 실행되고, 남은 이벤트는 더 넘기지 않는다.
    CHECK(closer.calls == 1);
    CHECK(!watcher.IsOpen());
    Recorder recorder;
    CHECK(watcher.Poll(&Recorder::Visit, &recorder).code == FileCode::NotOpen);
}

TEST(DirectoryWatch_ReportsRootRename) {
    Scratch scratch;
    REQUIRE(scratch.Make("rename-root"));
    char root[256];
    scratch.Child("watched", root, sizeof(root));
    REQUIRE(PhysicalFileSystem().MakeDirectory(root, strlen(root)));
    DirectoryWatcher watcher;
    REQUIRE(watcher.Open(root));
    char moved[256];
    scratch.Child("moved", moved, sizeof(moved));
    REQUIRE(PhysicalFileSystem().Move(root, strlen(root), moved, strlen(moved), false));
    Recorder recorder;
    // Wait가 true일 때만 Poll하는 소비자도 루트 이동을 알아야 한다.
    bool reported = false;
    const uint64 deadline = MonotonicNanoseconds() + 5000000000ull;
    while (!reported && MonotonicNanoseconds() < deadline) {
        if (watcher.Wait(100)) {
            REQUIRE(watcher.Poll(&Recorder::Visit, &recorder));
            reported = recorder.HasKind(DirectoryChangeKind::RootRemoved);
        }
    }
    CHECK(reported);
    watcher.Close();
}

TEST(DirectoryWatch_ReportsRootRemoval) {
    Scratch scratch;
    REQUIRE(scratch.Make("root"));
    char root[256];
    scratch.Child("watched", root, sizeof(root));
    REQUIRE(PhysicalFileSystem().MakeDirectory(root, strlen(root)));
    DirectoryWatcher watcher;
    REQUIRE(watcher.Open(root));
    Recorder recorder;
    REQUIRE(PhysicalFileSystem().DeleteDirectory(root, strlen(root)));
    CHECK(PollUntil(watcher, recorder,
                    [&] { return recorder.HasKind(DirectoryChangeKind::RootRemoved); }));
    // 루트 삭제 뒤에는 기다릴 것이 없다.
    CHECK(!watcher.Wait(10) || watcher.Poll(&Recorder::Visit, &recorder));
}

TEST(DirectoryWatch_QueueCollapsesToOverflowAtLimit) {
    detail::WatchEventQueue queue;
    char path[200];
    usize pushed = 0;
    // 상한(4 MiB)을 넘을 때까지 넣으면 앞선 이벤트를 모두 버리고 루트 Overflow 하나만 남긴다.
    // 레코드 하나가 약 100 byte이므로 10만 개면 상한을 넘는다.
    for (; pushed < 100000; ++pushed) {
        const int bytes =
            snprintf(path, sizeof(path), "directory/with/a/longer/name/entry-%08llu-padding.txt",
                     static_cast<unsigned long long>(pushed));
        if (!queue.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File, path,
                        static_cast<usize>(bytes))) {
            break;
        }
    }
    CHECK(pushed < 100000);
    DirectoryChange change;
    REQUIRE(queue.Front(change));
    CHECK(change.kind == DirectoryChangeKind::Overflow && change.pathBytes == 0);
    // 루트 Overflow 뒤의 이벤트는 그 재검색에 포함되어 합쳐지고, 루트 소실은 항상 마지막에 남는다.
    CHECK(queue.Push(DirectoryChangeKind::Modified, DirectoryEntryKind::File, "x", 1));
    CHECK(queue.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "", 0));
    queue.Pop();
    REQUIRE(queue.Front(change));
    CHECK(change.kind == DirectoryChangeKind::RootRemoved);
    queue.Pop();
    CHECK(queue.IsEmpty());

    // 하위 트리 Overflow는 경로를 유지한다.
    CHECK(queue.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Directory, "sub", 3));
    REQUIRE(queue.Front(change));
    CHECK(change.kind == DirectoryChangeKind::Overflow && strcmp(change.path, "sub") == 0);
}
