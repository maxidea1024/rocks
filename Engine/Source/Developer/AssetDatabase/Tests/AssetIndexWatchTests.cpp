#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetDatabaseTestSupport.h"
#include "AssetIndexCompare.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/AssetIndexWatch.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/NFailAllocator.h>

// 색인 부분 갱신·감시 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 16 A2d3): RefreshPaths(파일·하위
// 트리·중복·덮음·이름 규칙·쓰는 중 미루기·정규화 대조·할당 실패 — 결과는 지금 파일 상태의 새
// 검색과 같다), RootLost, 감시(시작·정지, 이벤트 종류 × 항목 종류 대응, Overflow·원천 오류·잘못된
// 이름, 루트 소실·재열기 간격·시계 역행, 쓰는 중 대기의 안정·상한·사라짐, 할당 실패 뒤 다시 하기),
// 실제 감시기 한 번.
using namespace rocks;
namespace asset_index_watch_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;
using platform::DirectoryChangeKind;
using platform::DirectoryEntryKind;

namespace {

constexpr int64 StartTicks = 1000 * AssetClockTicksPerSecond;
constexpr uint64 BadTagsId = 2960;
constexpr int64 Millisecond = AssetClockTicksPerMillisecond;

// 감시 장면이다: 루트는 디스크 임시 디렉터리, 파일은 그 루트에 놓인 메모리 파일 시스템, Content에
// Base·Hero·Sub/Leaf.
struct WatchScene {
    WatchScene(AssetEnv& env, Sources& sources, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddDirectory("Content") && files.AddDirectory("Content/Sub") &&
                files.AddFile("Content/Base.rasset", sources.base.View()) &&
                files.AddFile("Content/Hero.rasset", sources.hero.View()) &&
                files.AddFile("Content/Sub/Leaf.rasset", sources.leaf.View());
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& project) noexcept {
        return layout.Initialize(project.Root()) ? layout.Root() : project.Root();
    }

    // relative(메모리 루트 상대) 파일을 bytes로 바꾼다(없으면 만든다).
    bool Put(StringView relative, Span<const uint8> bytes,
             MemoryFault fault = MemoryFault::None) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, bytes, fault);
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    bool ready = false;
};

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

// bytes의 앞 절반이다(쓰는 중인 파일 — CorruptPackage).
Span<const uint8> Half(const Array<uint8>& bytes) noexcept {
    return Span<const uint8>(bytes.Data(), bytes.Size() / 2);
}

// index가 지금 파일 상태의 새 검색과 같은지다.
bool MatchesRebuild(AssetEnv& env, WatchScene& scene, const AssetIndex& index) noexcept {
    AssetIndex fresh(env.allocator, scene.layout);
    return fresh.Rebuild(scene.files, ReadOptions()) && SameIndex(fresh, index);
}

const AssetIndexEntry* EntryOf(const AssetIndex& index, StringView fileName) noexcept {
    for (usize at = 0; at < index.Count(); ++at) {
        if (index.At(at).fileName.View() == fileName) {
            return &index.At(at);
        }
    }

    return nullptr;
}

usize DiagnosticsOf(const AssetIndex& index, StringView fileName,
                    AssetDiagnosticCode code) noexcept {
    usize count = 0;
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        count += diagnostic.fileName.View() == fileName && diagnostic.code == code ? 1 : 0;
    }

    return count;
}

// index를 into에 복사한 것과 같은 상태를 다시 만든다(실패한 갱신이 색인을 바꾸지 않았는지
// 단언하는 기준 — 같은 파일 상태의 새 검색).
bool Snapshot(WatchScene& scene, AssetIndex& into) noexcept {
    return static_cast<bool>(into.Rebuild(scene.files, ReadOptions()));
}

// 시험 원천이다: Push로 쌓은 이벤트를 Poll이 넘긴다(visitor가 거짓이면 그 이벤트부터 남긴다).
// 열기·Poll 오류와 호출 수를 고를 수 있다. 경로는 byte 그대로다(잘못된 UTF-8 포함).
class FakeChangeSource final : public AssetChangeSource {
public:
    explicit FakeChangeSource(Allocator& allocator) noexcept
        : root(allocator), allocator_(&allocator), events_(allocator) {
    }

    bool Push(DirectoryChangeKind kind, DirectoryEntryKind entry, StringView path,
              StringView oldPath = StringView()) noexcept {
        Event event(*allocator_);
        event.kind = kind;
        event.entry = entry;
        return event.path.TryAppend(Span<const char>(path.Data(), path.ByteCount())) &&
               event.path.TryPushBack('\0') &&
               event.oldPath.TryAppend(Span<const char>(oldPath.Data(), oldPath.ByteCount())) &&
               event.oldPath.TryPushBack('\0') && events_.TryPushBack(Move(event));
    }

    usize Pending() const noexcept {
        return events_.Size();
    }

    Result<void> Open(StringView where) noexcept override {
        ++opens;
        if (openError != ErrorCode::None) {
            return Result<void>::Failure(openError);
        }

        isOpen = true;
        return root.TryAssign(where);
    }

    Result<void> Poll(platform::DirectoryChangeVisitor visitor, void* context) noexcept override {
        ++polls;
        usize consumed = 0;
        for (; consumed < events_.Size(); ++consumed) {
            const Event& event = events_[consumed];
            platform::DirectoryChange change;
            change.kind = event.kind;
            change.entry = event.entry;
            change.path = event.path.Data();
            change.pathBytes = event.path.Size() - 1;
            change.oldPath = event.oldPath.Data();
            change.oldPathBytes = event.oldPath.Size() - 1;
            if (!visitor(change, context)) {
                break;
            }
        }

        for (usize at = 0; at < consumed; ++at) {
            (void)events_.TryRemoveAt(0);
        }

        return pollError == ErrorCode::None ? Result<void>::Success()
                                            : Result<void>::Failure(pollError);
    }

    void Close() noexcept override {
        ++closes;
        isOpen = false;
        events_.Clear();
    }

    ErrorCode openError = ErrorCode::None;
    ErrorCode pollError = ErrorCode::None;
    bool isOpen = false;
    uint32 opens = 0;
    uint32 polls = 0;
    uint32 closes = 0;
    String root;

private:
    struct Event {
        explicit Event(Allocator& allocator) noexcept : path(allocator), oldPath(allocator) {
        }

        DirectoryChangeKind kind = DirectoryChangeKind::Modified;
        DirectoryEntryKind entry = DirectoryEntryKind::Unknown;
        Array<char> path;
        Array<char> oldPath;
    };

    Allocator* allocator_;
    Array<Event> events_;
};

} // namespace

// 파일 갱신: 내용 바뀜·새 파일·없어짐·디렉터리가 됨·같은 이름 여러 번을 한 번에 다시 읽으면 지금
// 파일 상태의 새 검색과 같다(같은 id 진단도 전체에서 다시 정한다 — 생기고 사라짐). 바뀌지 않은
// 이름도 넣을 수 있다. 같은 이름은 한 번 읽는다(열기 수). Refresh(한 파일)도 options.types를
// 쓰지 않는다.
TEST(AssetIndexRefreshPaths_FilesMatchRebuild) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "refresh-files");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));

    // 같은 id(Copy = Leaf)가 생긴다.
    REQUIRE(scene.Put("Content/Hero.rasset", sources.villain.View()));
    REQUIRE(scene.Put("Content/Copy.rasset", sources.leaf.View()));
    scene.files.Remove("Content/Base.rasset");
    REQUIRE(scene.files.AddDirectory("Content/Dir.rasset"));
    const StringView changed[] = {"Hero.rasset", "Copy.rasset", "Base.rasset",
                                  "Copy.rasset", "Dir.rasset",  "Sub/Leaf.rasset"};
    AssetIndexRefreshPaths paths;
    paths.files = changed;
    const uint32 opensBefore = scene.files.opens;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    // 다시 읽은 파일은 넷(Hero·Copy·Leaf 열기 — 없는 Base·디렉터리 Dir은 열지 않는다, Copy 한 번).
    CHECK(scene.files.opens - opensBefore == 3);
    CHECK(MatchesRebuild(env, scene, index));
    CHECK(DiagnosticsOf(index, "Copy.rasset", AssetDiagnosticCode::DuplicateAssetId) == 1);
    CHECK(EntryOf(index, "Base.rasset") == nullptr);

    // 같은 id가 사라진다.
    scene.files.Remove("Content/Copy.rasset");
    const StringView removed[] = {"Copy.rasset"};
    paths.files = removed;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(MatchesRebuild(env, scene, index));
    CHECK(DiagnosticsOf(index, "Sub/Leaf.rasset", AssetDiagnosticCode::DuplicateAssetId) == 0);

    // 빈 입력은 그대로 다시 게시한다.
    REQUIRE(index.RefreshPaths(scene.files, AssetIndexRefreshPaths{}, ReadOptions()));
    CHECK(MatchesRebuild(env, scene, index));

    // 파일 IO 진단(Stat·읽기 실패)도 새 검색과 같다.
    REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View(), MemoryFault::Read));
    const StringView hero[] = {"Hero.rasset"};
    paths.files = hero;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(MatchesRebuild(env, scene, index));
    CHECK(DiagnosticsOf(index, "Hero.rasset", AssetDiagnosticCode::None) == 1);
    scene.files.faults.stat = MemoryError;
    scene.files.faults.statPath = "Content/Hero.rasset";
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    scene.files.faults.stat = {};
    scene.files.faults.statPath = nullptr;
    CHECK(DiagnosticsOf(index, "Hero.rasset", AssetDiagnosticCode::None) == 1 &&
          EntryOf(index, "Hero.rasset") == nullptr);
    REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View()));
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(MatchesRebuild(env, scene, index));

    // Refresh는 TypeRegistry를 쓰지 않는다(RefreshPaths·Rebuild와 같다).
    PackageReadOptions typed = ReadOptions();
    typed.types = &env.types;
    REQUIRE(index.Refresh(scene.files, "Hero.rasset", typed));
    CHECK(MatchesRebuild(env, scene, index));
}

// 하위 트리: 새 디렉터리(중첩)·없어진 디렉터리·디렉터리가 파일이 됨·이름이 패키지 파일·그 밖의
// 파일·링크·없는 이름·열거 실패·Stat 실패·Content 전체(빈 이름)·Content 열거 실패(오류, 색인
// 그대로)·트리 안 트리와 트리 안 파일(한 번)·그 아래 진단이 빠짐(유효한 이름과 잘못된 UTF-8 이름
// 아래 — 표시 escape가 다른 둘 다).
TEST(AssetIndexRefreshPaths_Subtrees) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "refresh-subtrees");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    AssetIndexRefreshPaths paths;
    auto refresh = [&](Span<const StringView> trees) noexcept {
        paths.subtrees = trees;
        return static_cast<bool>(index.RefreshPaths(scene.files, paths, ReadOptions()));
    };

    // 새 디렉터리(중첩)와 그 아래 손상 파일, 트리 안 트리·파일은 한 번만 읽는다.
    REQUIRE(scene.files.AddDirectory("Content/New"));
    REQUIRE(scene.files.AddDirectory("Content/New/Deep"));
    REQUIRE(scene.files.AddFile("Content/New/A.rasset", sources.villain.View()));
    REQUIRE(scene.files.AddFile("Content/New/Deep/B.rasset", sources.derived.View()));
    REQUIRE(scene.files.AddFile("Content/New/Deep/Junk.rasset", Half(sources.leaf)));
    const StringView newTrees[] = {"New/Deep", "New", "New"};
    const StringView newFiles[] = {"New/A.rasset"};
    paths.files = newFiles;
    const uint32 opensBefore = scene.files.opens;
    REQUIRE(refresh(newTrees));
    CHECK(scene.files.opens - opensBefore == 3);
    CHECK(MatchesRebuild(env, scene, index));
    CHECK(DiagnosticsOf(index, "New/Deep/Junk.rasset", AssetDiagnosticCode::CorruptPackage) == 1);
    paths.files = {};

    // 없어진 디렉터리: 그 아래 항목·진단이 빠진다.
    scene.files.Remove("Content/New/Deep/Junk.rasset");
    scene.files.Remove("Content/New/Deep/B.rasset");
    scene.files.Remove("Content/New/Deep");
    const StringView deep[] = {"New/Deep"};
    REQUIRE(refresh(deep));
    CHECK(MatchesRebuild(env, scene, index));
    CHECK(EntryOf(index, "New/Deep/B.rasset") == nullptr && EntryOf(index, "New/A.rasset"));

    // 디렉터리가 파일(패키지가 아님)이 됨 → 그 아래 없음, 이름이 패키지 파일 → 그 파일.
    scene.files.Remove("Content/New/A.rasset");
    scene.files.Remove("Content/New");
    REQUIRE(scene.files.AddFile("Content/New", Half(sources.leaf)));
    const StringView newOnly[] = {"New"};
    REQUIRE(refresh(newOnly));
    CHECK(MatchesRebuild(env, scene, index));
    scene.files.Remove("Content/New");
    REQUIRE(scene.files.AddFile("Content/Pack.rasset", sources.villain.View()));
    const StringView pack[] = {"Pack.rasset"};
    REQUIRE(refresh(pack));
    CHECK(EntryOf(index, "Pack.rasset") != nullptr && MatchesRebuild(env, scene, index));

    // 링크 디렉터리·없는 이름 → 그 아래 없음.
    REQUIRE(scene.files.AddDirectory("Content/Linked", true));
    REQUIRE(scene.files.AddFile("Content/Linked/C.rasset", sources.leaf.View()));
    const StringView linkedAndMissing[] = {"Linked", "Missing"};
    REQUIRE(refresh(linkedAndMissing));
    CHECK(EntryOf(index, "Linked/C.rasset") == nullptr && MatchesRebuild(env, scene, index));

    // 열거 실패·Stat 실패는 그 이름의 진단이다(다시 읽으면 빠진다).
    REQUIRE(scene.files.AddDirectory("Content/Broken", false, MemoryFault::Enumerate));
    const StringView broken[] = {"Broken"};
    REQUIRE(refresh(broken));
    CHECK(DiagnosticsOf(index, "Broken", AssetDiagnosticCode::None) == 1 &&
          MatchesRebuild(env, scene, index));
    scene.files.Remove("Content/Broken");
    REQUIRE(refresh(broken));
    CHECK(DiagnosticsOf(index, "Broken", AssetDiagnosticCode::None) == 0);
    scene.files.faults.stat = MemoryError;
    scene.files.faults.statPath = "Content/Sub";
    const StringView sub[] = {"Sub"};
    REQUIRE(refresh(sub));
    scene.files.faults.stat = {};
    scene.files.faults.statPath = nullptr;
    CHECK(DiagnosticsOf(index, "Sub", AssetDiagnosticCode::None) == 1 &&
          EntryOf(index, "Sub/Leaf.rasset") == nullptr);
    REQUIRE(refresh(sub));
    CHECK(MatchesRebuild(env, scene, index));

    // 잘못된 UTF-8 이름 아래 진단(표시가 유효한 이름과 다르게 escape된다)도 트리가 덮는다.
    const char invalidDirectory[] = "Content/S\xc3\xbc"
                                    "b/\xff.rasset";
    REQUIRE(scene.files.AddDirectory("Content/S\xc3\xbc"
                                     "b"));
    REQUIRE(scene.files.AddFile(StringView(invalidDirectory), sources.leaf.View()));
    REQUIRE(scene.files.AddDirectory("Content/Plain"));
    REQUIRE(scene.files.AddDirectory(StringView("Content/Plain/\xfe")));
    const StringView invalidTrees[] = {"S\xc3\xbc"
                                       "b",
                                       "Plain"};
    REQUIRE(refresh(invalidTrees));
    CHECK(MatchesRebuild(env, scene, index));
    usize invalidPaths = 0;
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        invalidPaths += diagnostic.code == AssetDiagnosticCode::InvalidPath ? 1 : 0;
    }

    CHECK(invalidPaths == 2);
    scene.files.Remove(StringView(invalidDirectory));
    scene.files.Remove(StringView("Content/Plain/\xfe"));
    REQUIRE(refresh(invalidTrees));
    CHECK(MatchesRebuild(env, scene, index));
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        CHECK(diagnostic.code != AssetDiagnosticCode::InvalidPath);
    }

    // Content 전체(빈 이름 — 다른 트리·파일을 덮는다)는 새 검색과 같다.
    REQUIRE(scene.Put("Content/Hero.rasset", sources.villain.View()));
    const StringView all[] = {"Sub", "", "Sub"};
    const StringView heroFile[] = {"Hero.rasset"};
    paths.files = heroFile;
    REQUIRE(refresh(all));
    CHECK(MatchesRebuild(env, scene, index));
    paths.files = {};

    // Content 열거 실패는 오류이고 색인 그대로다.
    AssetIndex before(env.allocator, scene.layout);
    REQUIRE(Snapshot(scene, before));
    scene.files.Remove("Content");
    REQUIRE(scene.files.AddDirectory("Content", false, MemoryFault::Enumerate));
    const StringView rootOnly[] = {""};
    paths.subtrees = rootOnly;
    auto failed = index.RefreshPaths(scene.files, paths, ReadOptions());
    CHECK(!failed && failed.Error() != ErrorCode::OutOfMemory);
    CHECK(SameIndex(before, index));
}

// 이름 규칙 위반·mayDefer 길이 불일치·deferred 없음은 InvalidArgument이고 색인 그대로다.
TEST(AssetIndexRefreshPaths_RejectsInvalidInput) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "refresh-invalid");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    AssetIndex before(env.allocator, scene.layout);
    REQUIRE(Snapshot(scene, before));
    REQUIRE(scene.Put("Content/Hero.rasset", sources.villain.View()));
    auto rejected = [&](const AssetIndexRefreshPaths& paths) noexcept {
        auto result = index.RefreshPaths(scene.files, paths, ReadOptions());
        return !result && result.Error() == ErrorCode::InvalidArgument && SameIndex(before, index);
    };

    const StringView badFiles[] = {"",
                                   "/Hero.rasset",
                                   "Hero.rasset/",
                                   "Sub//Leaf.rasset",
                                   "./Hero.rasset",
                                   "../Hero.rasset",
                                   "Sub/./Leaf.rasset",
                                   "Sub/../Hero.rasset",
                                   "Hero.txt",
                                   ".rasset",
                                   StringView("\xff.rasset")};
    for (StringView file : badFiles) {
        const StringView one[] = {"Hero.rasset", file};
        AssetIndexRefreshPaths paths;
        paths.files = one;
        CHECK(rejected(paths));
    }

    const StringView badTrees[] = {"/Sub", "Sub/",   "Sub//Deep",       ".",
                                   "..",   "Sub/..", StringView("\xfe")};
    for (StringView tree : badTrees) {
        const StringView one[] = {"Sub", tree};
        AssetIndexRefreshPaths paths;
        paths.subtrees = one;
        CHECK(rejected(paths));
    }

    const StringView hero[] = {"Hero.rasset"};
    const bool two[] = {true, true};
    const bool one[] = {true};
    Array<usize> deferred(env.allocator);
    AssetIndexRefreshPaths mismatch;
    mismatch.files = hero;
    mismatch.mayDefer = two;
    mismatch.deferred = &deferred;
    CHECK(rejected(mismatch));
    AssetIndexRefreshPaths missing;
    missing.files = hero;
    missing.mayDefer = one;
    CHECK(rejected(missing));
}

// 쓰는 중 미루기: CorruptPackage·Busy인 mayDefer 파일은 지금 항목·진단을 그대로 두고 위치(같은
// 이름 모두)를 붙인다. 새 파일이면 아무것도 생기지 않는다. 같은 이름 중 하나라도 거짓이면·트리가
// 덮으면·그 밖의 실패(읽기 IO)면 미루지 않는다. mayDefer가 거짓이면 손상 진단을 남긴다.
TEST(AssetIndexRefreshPaths_Defers) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "refresh-defers");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    AssetIndex before(env.allocator, scene.layout);
    REQUIRE(Snapshot(scene, before));

    // 손상(쓰는 중)·Busy·새 파일 손상: 셋 다 미룬다. 같은 이름 둘은 위치 둘.
    REQUIRE(scene.Put("Content/Hero.rasset", Half(sources.hero)));
    REQUIRE(scene.Put("Content/Base.rasset", sources.base.View(), MemoryFault::OpenBusy));
    REQUIRE(scene.files.AddFile("Content/Writing.rasset", Half(sources.villain)));
    const StringView names[] = {"Hero.rasset", "Base.rasset", "Writing.rasset", "Hero.rasset"};
    const bool all[] = {true, true, true, true};
    Array<usize> deferred(env.allocator);
    AssetIndexRefreshPaths paths;
    paths.files = names;
    paths.mayDefer = all;
    paths.deferred = &deferred;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(deferred.Size() == 4);
    bool seen[4] = {};
    for (usize at : deferred) {
        seen[at] = true;
    }

    CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    CHECK(SameIndex(before, index));

    // 같은 이름 중 하나가 거짓이면 미루지 않는다(손상 진단이 남는다).
    const bool mixed[] = {true, true, true, false};
    deferred.Clear();
    paths.mayDefer = mixed;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(deferred.Size() == 2);
    CHECK(DiagnosticsOf(index, "Hero.rasset", AssetDiagnosticCode::CorruptPackage) == 1 &&
          EntryOf(index, "Hero.rasset") == nullptr);

    // 트리가 덮으면 미루지 않는다.
    deferred.Clear();
    const StringView root[] = {""};
    paths.subtrees = root;
    paths.mayDefer = all;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(deferred.IsEmpty() && MatchesRebuild(env, scene, index));
    paths.subtrees = {};

    // 그 밖의 실패(읽기 IO)는 미루지 않는다.
    REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View(), MemoryFault::Read));
    deferred.Clear();
    const StringView hero[] = {"Hero.rasset"};
    const bool yes[] = {true};
    paths.files = hero;
    paths.mayDefer = yes;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(deferred.IsEmpty() &&
          DiagnosticsOf(index, "Hero.rasset", AssetDiagnosticCode::None) == 1);

    // 완성되면 미루지 않고 바꾼다.
    REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View()));
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(deferred.IsEmpty() && EntryOf(index, "Hero.rasset") != nullptr);

    // 읽혀서 항목이 있는 파일의 손상 진단(Tags), 이름 규칙 위반(InvalidPath), 없는 파일(진단
    // 없음)은 다 쓴 상태라 미루지 않는다.
    RootRef<Package> badPackage;
    REQUIRE(!MakeAsset<TestAsset>(*env.source, BadTagsId, badPackage).IsNull());
    const Array<uint8> broken = OneFieldRecord(env.allocator, PackageTagsSchema);
    PackageWriteOptions badOptions = EditorOptions();
    badOptions.tags = broken.View();
    const Array<uint8> badTags = Write(env.allocator, *env.source, badPackage.Handle(), badOptions);
    REQUIRE(!badTags.IsEmpty());
    REQUIRE(scene.files.AddFile("Content/BadTags.rasset", badTags.View()));
    REQUIRE(scene.files.AddFile("Content/Bad Name.rasset", sources.leaf.View()));
    const StringView settled[] = {"BadTags.rasset", "Bad Name.rasset", "Gone.rasset"};
    const bool settledDefer[] = {true, true, true};
    paths.files = settled;
    paths.mayDefer = settledDefer;
    deferred.Clear();
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(deferred.IsEmpty() && EntryOf(index, "BadTags.rasset") != nullptr &&
          DiagnosticsOf(index, "BadTags.rasset", AssetDiagnosticCode::CorruptPackage) == 1 &&
          DiagnosticsOf(index, "Bad Name.rasset", AssetDiagnosticCode::InvalidPath) == 1);
}

// 정규화 대조: normalizesNames면 NFD 이벤트 이름이 NFC 항목·진단·하위 트리와 같은 이름이고(같은
// 키는 한 번 읽는다), 끄면 byte가 달라 다른 이름이다.
TEST(AssetIndexRefreshPaths_NormalizesNames) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "refresh-normalize");
    REQUIRE(scene.ready);
    const char nfcFile[] = "Content/Caf\xc3\xa9.rasset";
    const char nfcDirectory[] = "Content/Caf\xc3\xa9";
    const char nfcNested[] = "Content/Caf\xc3\xa9/Leaf2.rasset";
    const char nfcJunk[] = "Content/Caf\xc3\xa9/Junk.rasset";
    REQUIRE(scene.files.AddFile(StringView(nfcFile), sources.villain.View()));
    REQUIRE(scene.files.AddDirectory(StringView(nfcDirectory)));
    REQUIRE(scene.files.AddFile(StringView(nfcNested), sources.derived.View()));
    REQUIRE(scene.files.AddFile(StringView(nfcJunk), Half(sources.leaf)));
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    REQUIRE(index.Count() == 5);
    const StringView nfdFile = "Cafe\xcc\x81.rasset";
    const StringView nfdDirectory = "Cafe\xcc\x81";

    // 정규화하지 않으면 NFD 이름은 다른 파일이다(없으므로 아무것도 빠지지 않는다).
    const StringView nfdFiles[] = {nfdFile};
    const StringView nfdTrees[] = {nfdDirectory};
    AssetIndexRefreshPaths paths;
    paths.files = nfdFiles;
    paths.subtrees = nfdTrees;
    paths.normalizesNames = false;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(index.Count() == 5 && MatchesRebuild(env, scene, index));

    // 정규화하면 NFD 이름이 NFC 항목·진단을 가리킨다(메모리 파일 시스템은 정규화하지 않으므로
    // 다시 읽기가 없음을 본다 — 그 이름의 항목·진단이 빠진다).
    paths.normalizesNames = true;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(index.Count() == 3);
    CHECK(DiagnosticsOf(index, StringView(nfcJunk + 8), AssetDiagnosticCode::CorruptPackage) == 0);

    // 같은 키(NFC·NFD)는 한 번 읽는다(처음 철자 — NFC 파일).
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    const StringView both[] = {StringView(nfcFile + 8), nfdFile};
    paths.files = both;
    paths.subtrees = {};
    const uint32 opensBefore = scene.files.opens;
    REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    CHECK(scene.files.opens - opensBefore == 1 && MatchesRebuild(env, scene, index));
}

// 할당 실패: 파일·트리·미루기를 섞은 갱신의 모든 할당 실패는 OutOfMemory이고 색인 그대로다.
// 실패가 없어지면 실패 없는 기준 실행과 같다. MarkRootLost의 할당 실패도 색인 그대로다.
TEST(AssetIndexRefreshPaths_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "refresh-allocation");
    REQUIRE(scene.ready);
    REQUIRE(scene.files.AddFile("Content/Junk.rasset", Half(sources.leaf)));
    AssetIndex before(env.allocator, scene.layout);
    REQUIRE(Snapshot(scene, before));
    REQUIRE(scene.Put("Content/Hero.rasset", Half(sources.hero)));
    REQUIRE(scene.Put("Content/Base.rasset", sources.villain.View()));
    REQUIRE(scene.files.AddDirectory("Content/Sub/Deep"));
    REQUIRE(scene.files.AddFile("Content/Sub/Deep/D.rasset", sources.derived.View()));
    const StringView names[] = {"Hero.rasset", "Base.rasset", "Junk.rasset", "Hero.rasset"};
    const bool mayDefer[] = {true, false, false, true};
    const StringView trees[] = {"Sub", "Sub/Deep"};
    auto run = [&](AssetIndex& index, Array<usize>& deferred) noexcept {
        AssetIndexRefreshPaths paths;
        paths.files = names;
        paths.subtrees = trees;
        paths.mayDefer = mayDefer;
        paths.deferred = &deferred;
        paths.normalizesNames = true;
        return index.RefreshPaths(scene.files, paths, ReadOptions());
    };

    // 기준: 실패 전 상태(before와 같은 파일 상태의 색인)에서 실패 없이 갱신한다.
    AssetIndex expected(env.allocator, scene.layout);
    {
        REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View()));
        REQUIRE(scene.Put("Content/Base.rasset", sources.base.View()));
        scene.files.Remove("Content/Sub/Deep/D.rasset");
        scene.files.Remove("Content/Sub/Deep");
        REQUIRE(expected.Rebuild(scene.files, ReadOptions()));
        REQUIRE(scene.Put("Content/Hero.rasset", Half(sources.hero)));
        REQUIRE(scene.Put("Content/Base.rasset", sources.villain.View()));
        REQUIRE(scene.files.AddDirectory("Content/Sub/Deep"));
        REQUIRE(scene.files.AddFile("Content/Sub/Deep/D.rasset", sources.derived.View()));
        Array<usize> deferred(env.allocator);
        REQUIRE(run(expected, deferred));
        CHECK(deferred.Size() == 2);
    }

    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex index(failing, scene.layout);
        {
            REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View()));
            REQUIRE(scene.Put("Content/Base.rasset", sources.base.View()));
            scene.files.Remove("Content/Sub/Deep/D.rasset");
            scene.files.Remove("Content/Sub/Deep");
            REQUIRE(index.Rebuild(scene.files, ReadOptions()));
            REQUIRE(scene.Put("Content/Hero.rasset", Half(sources.hero)));
            REQUIRE(scene.Put("Content/Base.rasset", sources.villain.View()));
            REQUIRE(scene.files.AddDirectory("Content/Sub/Deep"));
            REQUIRE(scene.files.AddFile("Content/Sub/Deep/D.rasset", sources.derived.View()));
        }

        Array<usize> deferred(env.allocator);
        failing.FailOnAttempt(attempt);
        auto refreshed = run(index, deferred);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(refreshed);
            CHECK(SameIndex(expected, index));
            break;
        }

        CHECK(!refreshed && refreshed.Error() == ErrorCode::OutOfMemory);
        CHECK(SameIndex(before, index));
        ++failures;
    }

    CHECK(failures > 0);

    // MarkRootLost.
    AssetIndex lost(failing, scene.layout);
    REQUIRE(lost.Rebuild(scene.files, ReadOptions()));
    const usize count = lost.Count();
    failing.FailOnAttempt(1);
    auto marked = lost.MarkRootLost();
    failing.DisableFailure();
    CHECK(!marked && marked.Error() == ErrorCode::OutOfMemory && lost.Count() == count);
}

// RootLost: 항목을 모두 빼고 진단 하나(이름 빈 문자열, NotFound), 조회는 NotFound, 진단 줄·이름.
TEST(AssetIndex_MarkRootLost) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "root-lost");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    REQUIRE(index.Count() == 3);
    REQUIRE(index.MarkRootLost());
    CHECK(index.Count() == 0 && index.Diagnostics().Size() == 1);
    const AssetIndexDiagnostic& lost = index.Diagnostics()[0];
    CHECK(lost.code == AssetDiagnosticCode::RootLost && lost.error == ErrorCode::NotFound &&
          lost.fileName.View().IsEmpty());
    auto found = index.FindById(FixedId(HeroId));
    CHECK(!found && found.Error() == ErrorCode::NotFound);
    StringBuilder<256> line(env.allocator);
    REQUIRE(AppendAssetIndexDiagnosticLine(line, lost));
    CHECK(line.View() == "RootLost: Content/: the content directory disappeared\n");
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::RootLost)) == "RootLost");
    CHECK(&index.Layout() == &scene.layout);
}

// 시작·정지: Start는 원천을 Content에 열고 다시 검색한다(캐시 사용). 두 번 Start·Start 전 Pump는
// InvalidArgument, 원천 열기 실패는 그 오류(시작 안 함), Rebuild 실패는 원천을 닫는다. Stop은
// 닫고 여러 번 불러도 된다, 소멸자도 닫는다. 이벤트 없는 Pump는 아무것도 하지 않는다.
TEST(AssetIndexWatch_StartsAndStops) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "watch-start");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    FakeChangeSource source(env.allocator);
    ManualAssetClock clock(StartTicks);
    {
        AssetIndexWatch watch(env.allocator, index, scene.files, ReadOptions(), source, clock);
        auto early = watch.Pump();
        CHECK(!early && early.Error() == ErrorCode::InvalidArgument);
        source.openError = ErrorCode::NotFound;
        auto missing = watch.Start();
        CHECK(!missing && missing.Error() == ErrorCode::NotFound && !watch.IsStarted());
        source.openError = ErrorCode::None;

        // Rebuild 실패(Content 열거).
        scene.files.Remove("Content");
        REQUIRE(scene.files.AddDirectory("Content", false, MemoryFault::Enumerate));
        auto unreadable = watch.Start();
        CHECK(!unreadable && !watch.IsStarted() && !source.isOpen && source.closes == 1);
        scene.files.Remove("Content");
        REQUIRE(scene.files.AddDirectory("Content"));

        // 캐시를 쓴 시작.
        AssetIndex plain(env.allocator, scene.layout);
        REQUIRE(plain.Rebuild(scene.files, ReadOptions()));
        Array<uint8> bytes(env.allocator);
        REQUIRE(EncodeAssetIndexCache(plain, MakeAssetIndexCacheKey(ReadOptions()), bytes));
        AssetIndexCache cache(env.allocator);
        REQUIRE(cache.Parse(bytes.View(), MakeAssetIndexCacheKey(ReadOptions())));
        REQUIRE(watch.Start(&cache));
        CHECK(watch.IsStarted() && source.isOpen &&
              source.root.View() == scene.layout.ContentDirectory());
        CHECK(MatchesRebuild(env, scene, index));
        auto twice = watch.Start();
        CHECK(!twice && twice.Error() == ErrorCode::InvalidArgument);
        REQUIRE(watch.Pump());
        CHECK(source.polls == 1 && MatchesRebuild(env, scene, index));
        watch.Stop();
        CHECK(!watch.IsStarted() && !source.isOpen && source.closes == 2);
        watch.Stop();
        CHECK(source.closes == 2);
        REQUIRE(watch.Start());
    }

    CHECK(!source.isOpen && source.closes == 3);
}

// 이벤트 대응(종류 × 항목 종류 × 이름): `.rasset`이면 파일(디렉터리면 빼고), 디렉터리·모름이면
// 하위 트리, 그 밖의 파일은 무시, Renamed는 옛·새 이름 둘 다. 결과는 파일 상태로 단언한다(무시한
// 이벤트는 색인을 바꾸지 않는다).
TEST(AssetIndexWatch_MapsEvents) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "watch-events");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    FakeChangeSource source(env.allocator);
    ManualAssetClock clock(StartTicks);
    AssetIndexWatch watch(env.allocator, index, scene.files, ReadOptions(), source, clock);
    REQUIRE(watch.Start());
    const DirectoryChangeKind kinds[] = {DirectoryChangeKind::Added, DirectoryChangeKind::Removed,
                                         DirectoryChangeKind::Modified};
    for (DirectoryChangeKind kind : kinds) {
        // `.rasset` + 파일·모름 → 파일을 읽는다.
        const DirectoryEntryKind fileLike[] = {DirectoryEntryKind::File,
                                               DirectoryEntryKind::Unknown};
        for (DirectoryEntryKind entry : fileLike) {
            REQUIRE(scene.Put("Content/Event.rasset", sources.villain.View()));
            REQUIRE(source.Push(kind, entry, "Event.rasset"));
            REQUIRE(watch.Pump());
            CHECK(EntryOf(index, "Event.rasset") != nullptr && MatchesRebuild(env, scene, index));
            scene.files.Remove("Content/Event.rasset");
            REQUIRE(source.Push(kind, entry, "Event.rasset"));
            REQUIRE(watch.Pump());
            CHECK(EntryOf(index, "Event.rasset") == nullptr && MatchesRebuild(env, scene, index));
        }

        // `.rasset` + 디렉터리 → 하위 트리만(그 아래 패키지).
        REQUIRE(scene.files.AddDirectory("Content/Folder.rasset"));
        REQUIRE(scene.files.AddFile("Content/Folder.rasset/In.rasset", sources.derived.View()));
        REQUIRE(source.Push(kind, DirectoryEntryKind::Directory, "Folder.rasset"));
        REQUIRE(watch.Pump());
        // 디렉터리 이름의 '.'은 논리 경로 규칙 위반이라 그 아래 파일은 InvalidPath 진단이다(새
        // 검색과 같다 — 읽혔다는 증거).
        CHECK(DiagnosticsOf(index, "Folder.rasset/In.rasset", AssetDiagnosticCode::InvalidPath) ==
                  1 &&
              MatchesRebuild(env, scene, index));
        scene.files.Remove("Content/Folder.rasset/In.rasset");
        scene.files.Remove("Content/Folder.rasset");
        REQUIRE(source.Push(kind, DirectoryEntryKind::Unknown, "Folder.rasset"));
        REQUIRE(watch.Pump());
        CHECK(MatchesRebuild(env, scene, index));

        // 그 밖의 이름 + 디렉터리·모름 → 하위 트리.
        const DirectoryEntryKind treeLike[] = {DirectoryEntryKind::Directory,
                                               DirectoryEntryKind::Unknown};
        for (DirectoryEntryKind entry : treeLike) {
            REQUIRE(scene.files.AddDirectory("Content/Folder"));
            REQUIRE(scene.files.AddFile("Content/Folder/In.rasset", sources.derived.View()));
            REQUIRE(source.Push(kind, entry, "Folder"));
            REQUIRE(watch.Pump());
            CHECK(EntryOf(index, "Folder/In.rasset") != nullptr &&
                  MatchesRebuild(env, scene, index));
            scene.files.Remove("Content/Folder/In.rasset");
            scene.files.Remove("Content/Folder");
            REQUIRE(source.Push(kind, entry, "Folder"));
            REQUIRE(watch.Pump());
            CHECK(MatchesRebuild(env, scene, index));
        }

        // 그 밖의 이름 + 파일 → 무시(임시 파일 등 — 이름이 디렉터리여도 보지 않는다).
        REQUIRE(scene.files.AddDirectory("Content/Ignored"));
        REQUIRE(scene.files.AddFile("Content/Ignored/In.rasset", sources.derived.View()));
        REQUIRE(source.Push(kind, DirectoryEntryKind::File, "Ignored"));
        REQUIRE(source.Push(kind, DirectoryEntryKind::File,
                            "Hero.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp"));
        REQUIRE(watch.Pump());
        CHECK(EntryOf(index, "Ignored/In.rasset") == nullptr);
        REQUIRE(source.Push(kind, DirectoryEntryKind::Directory, "Ignored"));
        REQUIRE(watch.Pump());
        scene.files.Remove("Content/Ignored/In.rasset");
        scene.files.Remove("Content/Ignored");
        REQUIRE(source.Push(kind, DirectoryEntryKind::Directory, "Ignored"));
        REQUIRE(watch.Pump());
        CHECK(MatchesRebuild(env, scene, index));
    }

    // Renamed: 옛 이름(빠짐)과 새 이름(더함) 둘 다 — 파일과 디렉터리.
    scene.files.Remove("Content/Base.rasset");
    REQUIRE(scene.files.AddFile("Content/Moved.rasset", sources.base.View()));
    REQUIRE(source.Push(DirectoryChangeKind::Renamed, DirectoryEntryKind::File, "Moved.rasset",
                        "Base.rasset"));
    REQUIRE(watch.Pump());
    CHECK(EntryOf(index, "Base.rasset") == nullptr && EntryOf(index, "Moved.rasset") != nullptr &&
          MatchesRebuild(env, scene, index));
    scene.files.Remove("Content/Sub/Leaf.rasset");
    scene.files.Remove("Content/Sub");
    REQUIRE(scene.files.AddDirectory("Content/Other"));
    REQUIRE(scene.files.AddFile("Content/Other/Leaf.rasset", sources.leaf.View()));
    REQUIRE(
        source.Push(DirectoryChangeKind::Renamed, DirectoryEntryKind::Directory, "Other", "Sub"));
    REQUIRE(watch.Pump());
    CHECK(EntryOf(index, "Sub/Leaf.rasset") == nullptr &&
          EntryOf(index, "Other/Leaf.rasset") != nullptr && MatchesRebuild(env, scene, index));
}

// Overflow(경로)는 그 하위 트리, Overflow(빈 경로)·원천 Poll 오류·규칙에 안 맞는 경로·첫 구성
// 요소가 잘못된 UTF-8인 경로는 전체, 중간이 잘못된 UTF-8이면 그 앞 디렉터리다. 전체인지는 이벤트
// 없는 다른 곳의 변경이 보이는지로 본다.
TEST(AssetIndexWatch_RescansOnOverflow) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "watch-overflow");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    FakeChangeSource source(env.allocator);
    ManualAssetClock clock(StartTicks);
    AssetIndexWatch watch(env.allocator, index, scene.files, ReadOptions(), source, clock);
    REQUIRE(watch.Start());

    // 하위 트리: Sub 아래 변경은 보이고 다른 곳(Silent)은 보이지 않는다.
    REQUIRE(scene.files.AddFile("Content/Sub/New.rasset", sources.villain.View()));
    REQUIRE(scene.files.AddFile("Content/Silent.rasset", sources.derived.View()));
    REQUIRE(source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "Sub"));
    REQUIRE(watch.Pump());
    CHECK(EntryOf(index, "Sub/New.rasset") != nullptr &&
          EntryOf(index, "Silent.rasset") == nullptr);

    // 전체로 접히는 것들: 각각 다른 곳의 조용한 변경이 보인다.
    auto fullRescan = [&](auto&& trigger) noexcept {
        scene.files.Remove("Content/Silent.rasset");
        if (!watch.Pump() ||
            !scene.files.AddFile("Content/Silent.rasset", sources.derived.View())) {
            return false;
        }

        // 먼저 Silent가 빠진 색인을 만든다(전체 다시 검색으로 확인할 변경).
        const StringView silent[] = {"Silent.rasset"};
        scene.files.Remove("Content/Silent.rasset");
        AssetIndexRefreshPaths paths;
        paths.files = silent;
        if (!index.RefreshPaths(scene.files, paths, ReadOptions()) ||
            !scene.files.AddFile("Content/Silent.rasset", sources.derived.View())) {
            return false;
        }

        trigger();
        return watch.Pump() && EntryOf(index, "Silent.rasset") != nullptr &&
               MatchesRebuild(env, scene, index);
    };
    CHECK(fullRescan([&]() noexcept {
        (void)source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "");
    }));
    CHECK(fullRescan([&]() noexcept { source.pollError = ErrorCode::Busy; }));
    source.pollError = ErrorCode::None;
    CHECK(fullRescan([&]() noexcept {
        (void)source.Push(DirectoryChangeKind::Modified, DirectoryEntryKind::File, "/Hero.rasset");
    }));
    CHECK(fullRescan([&]() noexcept {
        (void)source.Push(DirectoryChangeKind::Modified, DirectoryEntryKind::File,
                          "Sub//Leaf.rasset");
    }));
    CHECK(fullRescan([&]() noexcept {
        (void)source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File,
                          StringView("\xff/A.rasset"));
    }));
    // 전체가 잡힌 뒤의 경로는 더하지 않는다(같은 Pump의 다음 이벤트).
    CHECK(fullRescan([&]() noexcept {
        (void)source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "");
        (void)source.Push(DirectoryChangeKind::Modified, DirectoryEntryKind::File, "Hero.rasset");
    }));

    // 중간이 잘못된 UTF-8이면 그 앞 디렉터리(Sub)만 다시 검색한다.
    REQUIRE(scene.files.AddFile(StringView("Content/Sub/\xff.rasset"), sources.leaf.View()));
    scene.files.Remove("Content/Silent.rasset");
    REQUIRE(source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File,
                        StringView("Sub/\xff.rasset")));
    REQUIRE(watch.Pump());
    usize invalid = 0;
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        invalid += diagnostic.code == AssetDiagnosticCode::InvalidPath ? 1 : 0;
    }

    CHECK(invalid == 1 && EntryOf(index, "Silent.rasset") != nullptr);
}

// 루트 소실: RootRemoved면 원천을 닫고 색인은 RootLost 하나, 같은 Poll의 뒤 이벤트는 버린다.
// 1초 전에는 다시 열지 않고, NotFound면 조용히 다음 간격, 그 밖의 원천 오류는 Pump 오류, 다시
// 검색 실패면 RootLost 그대로(원천은 열린 채 — 다음 간격에 검색만), 성공하면 전체 다시 검색.
// 시계가 뒤로 가면 다시 1초를 기다린다. MarkRootLost 할당 실패는 다음 Pump가 다시 한다(Poll 없이).
TEST(AssetIndexWatch_RecoversLostRoot) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "watch-root");
    REQUIRE(scene.ready);
    test::NFailAllocator failing(env.allocator);
    AssetIndex index(failing, scene.layout);
    FakeChangeSource source(env.allocator);
    ManualAssetClock clock(StartTicks);
    AssetIndexWatch watch(env.allocator, index, scene.files, ReadOptions(), source, clock);
    REQUIRE(watch.Start());

    // 할당 실패한 소실 적용은 다음 Pump가 Poll 없이 다시 한다.
    REQUIRE(source.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, ""));
    REQUIRE(source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File, "After.rasset"));
    failing.FailOnAttempt(1);
    auto failed = watch.Pump();
    failing.DisableFailure();
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory && !watch.IsRootLost());
    CHECK(!source.isOpen && index.Count() == 3);
    const uint32 polls = source.polls;
    REQUIRE(watch.Pump());
    CHECK(source.polls == polls && watch.IsRootLost() && index.Count() == 0);
    CHECK(index.Diagnostics().Size() == 1 &&
          index.Diagnostics()[0].code == AssetDiagnosticCode::RootLost);

    // 1초 전에는 열지 않는다.
    clock.Advance(AssetWatchRootRetryTicks - 1);
    const uint32 opens = source.opens;
    REQUIRE(watch.Pump());
    CHECK(source.opens == opens);

    // NotFound는 조용히 다음 간격.
    clock.Advance(1);
    source.openError = ErrorCode::NotFound;
    REQUIRE(watch.Pump());
    CHECK(source.opens == opens + 1 && watch.IsRootLost());
    REQUIRE(watch.Pump());
    CHECK(source.opens == opens + 1);

    // 그 밖의 원천 오류는 Pump 오류(그래도 다음 간격에 다시 연다).
    clock.Advance(AssetWatchRootRetryTicks);
    source.openError = ErrorCode::Unsupported;
    auto denied = watch.Pump();
    CHECK(!denied && denied.Error() == ErrorCode::Unsupported && watch.IsRootLost());
    source.openError = ErrorCode::None;

    // 시계가 뒤로 가면 기준을 당겨 다시 1초를 기다린다.
    clock.Advance(-AssetWatchRootRetryTicks * 5);
    REQUIRE(watch.Pump());
    CHECK(source.opens == opens + 2);
    clock.Advance(AssetWatchRootRetryTicks - 1);
    REQUIRE(watch.Pump());
    CHECK(source.opens == opens + 2);

    // 열렸지만 다시 검색이 실패하면 RootLost 그대로(원천은 열린 채, 다음 간격에 검색만).
    clock.Advance(1);
    scene.files.Remove("Content");
    REQUIRE(scene.files.AddDirectory("Content", false, MemoryFault::Enumerate));
    auto unreadable = watch.Pump();
    CHECK(!unreadable && watch.IsRootLost() && source.isOpen && source.opens == opens + 3);
    CHECK(index.Diagnostics().Size() == 1);
    scene.files.Remove("Content");
    REQUIRE(scene.files.AddDirectory("Content"));
    REQUIRE(scene.files.AddFile("Content/Back.rasset", sources.villain.View()));
    clock.Advance(AssetWatchRootRetryTicks);
    REQUIRE(watch.Pump());
    CHECK(!watch.IsRootLost() && source.opens == opens + 3 && MatchesRebuild(env, scene, index));

    // 다시 감시한다.
    REQUIRE(scene.files.AddFile("Content/Later.rasset", sources.leaf.View()));
    REQUIRE(source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File, "Later.rasset"));
    REQUIRE(watch.Pump());
    CHECK(MatchesRebuild(env, scene, index));
}

// 쓰는 중 대기: 이벤트로 다시 읽은 파일이 손상·Busy면 옛 항목을 두고 기다린다. 같은 관찰이
// 500 ms 떨어져 두 번이면 다시 읽어 결과(손상이어도)를 남긴다. 크기가 바뀌면 관찰을 다시 시작하고,
// 계속 바뀌어도 처음 본 뒤 10초면 다시 읽는다. 새 이벤트는 다시 미루되 처음 본 시각은 그대로다.
// 사라짐·디렉터리가 됨은 바로 다시 읽는다. 완성된 이벤트는 대기를 뺀다. 시계가 뒤로 가면 다시
// 500 ms를 기다린다. 전체 다시 검색이 덮으면 대기를 뺀다.
TEST(AssetIndexWatch_WaitsForWritingFiles) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "watch-writing");
    REQUIRE(scene.ready);
    AssetIndex index(env.allocator, scene.layout);
    FakeChangeSource source(env.allocator);
    ManualAssetClock clock(StartTicks);
    AssetIndexWatch watch(env.allocator, index, scene.files, ReadOptions(), source, clock);
    REQUIRE(watch.Start());
    AssetIndex original(env.allocator, scene.layout);
    REQUIRE(Snapshot(scene, original));
    auto modified = [&](StringView name) noexcept {
        return source.Push(DirectoryChangeKind::Modified, DirectoryEntryKind::File, name);
    };

    // 손상: 옛 항목을 두고 기다린다(같은 파일 이벤트 둘도 대기 하나) → 499 ms 같음은 아직 →
    // 500 ms에 다시 읽어 손상을 남긴다.
    REQUIRE(scene.Put("Content/Hero.rasset", Half(sources.hero)));
    REQUIRE(modified("Hero.rasset"));
    REQUIRE(modified("Hero.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1 && SameIndex(original, index));
    clock.Advance(499 * Millisecond);
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1 && SameIndex(original, index));
    clock.Advance(Millisecond);
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 &&
          DiagnosticsOf(index, "Hero.rasset", AssetDiagnosticCode::CorruptPackage) == 1);

    // 크기가 바뀌면 관찰을 다시 시작하고, 완성 이벤트가 오면 대기를 빼고 바꾼다.
    REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View()));
    REQUIRE(modified("Hero.rasset"));
    REQUIRE(watch.Pump());
    REQUIRE(scene.Put("Content/Base.rasset", Half(sources.base)));
    REQUIRE(modified("Base.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1);
    clock.Advance(400 * Millisecond);
    REQUIRE(scene.Put("Content/Base.rasset",
                      Span<const uint8>(sources.base.Data(), sources.base.Size() - 1)));
    REQUIRE(watch.Pump());
    clock.Advance(400 * Millisecond);
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1 && EntryOf(index, "Base.rasset") != nullptr);
    REQUIRE(scene.Put("Content/Base.rasset", sources.base.View()));
    REQUIRE(modified("Base.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 && MatchesRebuild(env, scene, index));

    // 계속 바뀌어도 10초면 다시 읽는다(새 이벤트도 처음 본 시각을 바꾸지 않는다).
    REQUIRE(scene.files.AddFile("Content/Growing.rasset",
                                Span<const uint8>(sources.villain.Data(), 10)));
    REQUIRE(source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File, "Growing.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1 && EntryOf(index, "Growing.rasset") == nullptr &&
          DiagnosticsOf(index, "Growing.rasset", AssetDiagnosticCode::CorruptPackage) == 0);
    for (usize step = 1; step < 20; ++step) {
        clock.Advance(AssetWatchWriteTimeoutTicks / 20);
        REQUIRE(scene.Put("Content/Growing.rasset",
                          Span<const uint8>(sources.villain.Data(), 10 + step)));
        REQUIRE(modified("Growing.rasset"));
        REQUIRE(watch.Pump());
        CHECK(watch.WaitingCount() == 1);
    }

    clock.Advance(AssetWatchWriteTimeoutTicks / 20);
    REQUIRE(scene.Put("Content/Growing.rasset", Span<const uint8>(sources.villain.Data(), 40)));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 &&
          DiagnosticsOf(index, "Growing.rasset", AssetDiagnosticCode::CorruptPackage) == 1);

    // Busy도 기다린다. 사라지면 바로 다시 읽는다(항목이 빠진다).
    REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View(), MemoryFault::OpenBusy));
    REQUIRE(modified("Hero.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1 && EntryOf(index, "Hero.rasset") != nullptr);
    scene.files.Remove("Content/Hero.rasset");
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 && EntryOf(index, "Hero.rasset") == nullptr &&
          MatchesRebuild(env, scene, index));

    // 디렉터리가 되면 바로 다시 읽는다.
    REQUIRE(scene.files.AddFile("Content/Shape.rasset", Half(sources.leaf)));
    REQUIRE(modified("Shape.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1);
    scene.files.Remove("Content/Shape.rasset");
    REQUIRE(scene.files.AddDirectory("Content/Shape.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 && MatchesRebuild(env, scene, index));

    // 링크가 되면 바로 다시 읽는다(색인하지 않는다).
    REQUIRE(scene.files.AddFile("Content/Linked.rasset", Half(sources.leaf)));
    REQUIRE(modified("Linked.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1);
    scene.files.Remove("Content/Linked.rasset");
    REQUIRE(scene.files.AddFile("Content/Linked.rasset", sources.leaf.View(), MemoryFault::None,
                                platform::FileType::File, true));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 && EntryOf(index, "Linked.rasset") == nullptr &&
          MatchesRebuild(env, scene, index));
    scene.files.Remove("Content/Linked.rasset");

    // 크기는 같고 수정 시각만 바뀌어도 관찰을 다시 시작한다.
    REQUIRE(scene.files.AddFile("Content/Touched.rasset", Half(sources.leaf)));
    REQUIRE(modified("Touched.rasset"));
    REQUIRE(watch.Pump());
    clock.Advance(400 * Millisecond);
    scene.files.modifiedShift = 5;
    REQUIRE(watch.Pump());
    clock.Advance(400 * Millisecond);
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1);
    clock.Advance(100 * Millisecond);
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0);
    scene.files.modifiedShift = 0;
    scene.files.Remove("Content/Touched.rasset");
    REQUIRE(
        source.Push(DirectoryChangeKind::Removed, DirectoryEntryKind::Unknown, "Touched.rasset"));
    REQUIRE(watch.Pump());

    // 하위 트리가 덮는 대기만 뺀다: 같은 이름·그 아래는 빼고, 접두만 같은 이름(구분자
    // 없음)·트리보다 짧은 이름은 남긴다.
    REQUIRE(scene.files.AddFile("Content/Sub/W.rasset", Half(sources.leaf)));
    REQUIRE(scene.files.AddFile("Content/Subway.rasset", Half(sources.leaf)));
    REQUIRE(scene.files.AddFile("Content/W.rasset", Half(sources.leaf)));
    REQUIRE(scene.files.AddFile("Content/Exact.rasset", Half(sources.leaf)));
    const StringView writers[] = {"Sub/W.rasset", "Subway.rasset", "W.rasset", "Exact.rasset"};
    for (StringView writer : writers) {
        REQUIRE(modified(writer));
    }

    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 4);
    REQUIRE(source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "Sub"));
    REQUIRE(
        source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "LongerDirectory"));
    REQUIRE(
        source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "Exact.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 2);
    CHECK(DiagnosticsOf(index, "Sub/W.rasset", AssetDiagnosticCode::CorruptPackage) == 1 &&
          DiagnosticsOf(index, "Exact.rasset", AssetDiagnosticCode::CorruptPackage) == 1 &&
          DiagnosticsOf(index, "W.rasset", AssetDiagnosticCode::CorruptPackage) == 0);
    for (StringView writer : writers) {
        String path(env.allocator);
        REQUIRE(path.TryAppend("Content/"));
        REQUIRE(path.TryAppend(writer));
        scene.files.Remove(path.View());
        REQUIRE(source.Push(DirectoryChangeKind::Removed, DirectoryEntryKind::Unknown, writer));
    }

    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 && MatchesRebuild(env, scene, index));

    // 시계가 뒤로 가면 다시 500 ms를 기다린다.
    REQUIRE(scene.files.AddFile("Content/Back.rasset", Half(sources.leaf)));
    REQUIRE(modified("Back.rasset"));
    REQUIRE(watch.Pump());
    clock.Advance(-10 * AssetClockTicksPerSecond);
    REQUIRE(watch.Pump());
    clock.Advance(499 * Millisecond);
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1);
    clock.Advance(Millisecond);
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0);

    // 전체 다시 검색이 덮으면 대기를 뺀다(검색 결과를 남긴다).
    REQUIRE(scene.files.AddFile("Content/Covered.rasset", Half(sources.leaf)));
    REQUIRE(modified("Covered.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1);
    REQUIRE(source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, ""));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 0 && MatchesRebuild(env, scene, index));

    // 정지는 대기를 버린다.
    REQUIRE(scene.Put("Content/Covered.rasset", Half(sources.villain)));
    REQUIRE(modified("Covered.rasset"));
    REQUIRE(watch.Pump());
    CHECK(watch.WaitingCount() == 1);
    watch.Stop();
    CHECK(watch.WaitingCount() == 0 && watch.PendingFiles() == 0 && watch.PendingSubtrees() == 0 &&
          !watch.FullRescanPending());
}

// 할당 실패: 대기 관찰·이벤트·하위 트리·Renamed·새 대기를 섞은 Pump의 모든 할당 실패는 그 Pump가
// OutOfMemory이고 색인 그대로, 다음 Pump가 모은 일을 다시 해 실패 없는 기준 실행과 같은 색인·대기
// 수가 된다. 이벤트 기록의 할당 실패는 전체 다시 검색으로 접혀 Pump는 성공한다(새 검색과 같다).
TEST(AssetIndexWatch_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    WatchScene scene(env, sources, "watch-allocation");
    REQUIRE(scene.ready);
    struct Outcome {
        bool failed = false;
        bool folded = false;
    };

    // 장면 하나를 돈다: 시작 → 첫 Pump(Hero·Writing 대기) → 600 ms 뒤 섞은 Pump(실패 지점) →
    // 실패했으면 다시.
    auto play = [&](Allocator& allocator, test::NFailAllocator* failing, uint64 attempt,
                    AssetIndex& index, usize& waiting, Outcome& outcome) noexcept {
        REQUIRE(scene.Put("Content/Hero.rasset", sources.hero.View()));
        REQUIRE(scene.Put("Content/Base.rasset", sources.base.View()));
        scene.files.Remove("Content/Moved.rasset");
        scene.files.Remove("Content/Writing.rasset");
        scene.files.Remove("Content/Fresh.rasset");
        scene.files.Remove("Content/New.rasset");
        scene.files.Remove("Content/Sub/Other.rasset");
        if (!scene.files.Has("Content/Sub/Leaf.rasset")) {
            REQUIRE(scene.files.AddFile("Content/Sub/Leaf.rasset", sources.leaf.View()));
        }

        FakeChangeSource source(env.allocator);
        ManualAssetClock clock(StartTicks);
        AssetIndexWatch watch(allocator, index, scene.files, ReadOptions(), source, clock);
        REQUIRE(watch.Start());
        REQUIRE(scene.Put("Content/Hero.rasset", Half(sources.hero)));
        REQUIRE(scene.files.AddFile("Content/Writing.rasset", Half(sources.villain)));
        REQUIRE(
            source.Push(DirectoryChangeKind::Modified, DirectoryEntryKind::File, "Hero.rasset"));
        REQUIRE(
            source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File, "Writing.rasset"));
        REQUIRE(watch.Pump());
        REQUIRE(watch.WaitingCount() == 2);
        clock.Advance(600 * Millisecond);
        REQUIRE(scene.Put("Content/Writing.rasset",
                          Span<const uint8>(sources.villain.Data(), sources.villain.Size() / 3)));
        REQUIRE(scene.files.AddFile("Content/New.rasset", sources.derived.View()));
        REQUIRE(scene.files.AddFile("Content/Fresh.rasset", Half(sources.leaf)));
        scene.files.Remove("Content/Sub/Leaf.rasset");
        REQUIRE(scene.files.AddFile("Content/Sub/Other.rasset", sources.leaf.View()));
        scene.files.Remove("Content/Base.rasset");
        REQUIRE(scene.files.AddFile("Content/Moved.rasset", sources.base.View()));
        REQUIRE(source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File, "New.rasset"));
        REQUIRE(source.Push(DirectoryChangeKind::Added, DirectoryEntryKind::File, "Fresh.rasset"));
        REQUIRE(source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "Sub"));
        REQUIRE(source.Push(DirectoryChangeKind::Renamed, DirectoryEntryKind::File, "Moved.rasset",
                            "Base.rasset"));
        AssetIndex before(env.allocator, scene.layout);
        REQUIRE(before.Rebuild(scene.files, ReadOptions()));
        if (failing != nullptr) {
            failing->FailOnAttempt(attempt);
        }

        auto pumped = watch.Pump();
        outcome.failed = !pumped;
        if (failing != nullptr) {
            outcome.folded = pumped && failing->Attempts() >= attempt;
            failing->DisableFailure();
        }

        if (!pumped) {
            CHECK(pumped.Error() == ErrorCode::OutOfMemory);
            CHECK(watch.WaitingCount() == 2);
            REQUIRE(watch.Pump());
        }

        waiting = watch.WaitingCount();
    };

    AssetIndex reference(env.allocator, scene.layout);
    usize referenceWaiting = 0;
    Outcome referenceOutcome;
    play(env.allocator, nullptr, 0, reference, referenceWaiting, referenceOutcome);
    // 기준: Hero는 기다림이 끝나 손상 진단, Writing은 크기가 바뀌어 계속 대기, Fresh는 새 대기.
    CHECK(referenceWaiting == 2);
    CHECK(DiagnosticsOf(reference, "Hero.rasset", AssetDiagnosticCode::CorruptPackage) == 1);
    CHECK(EntryOf(reference, "Moved.rasset") != nullptr &&
          EntryOf(reference, "Sub/Other.rasset") != nullptr &&
          EntryOf(reference, "New.rasset") != nullptr);

    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    uint32 folds = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex index(failing, scene.layout);
        usize waiting = 0;
        Outcome outcome;
        play(failing, &failing, attempt, index, waiting, outcome);
        if (!outcome.failed && !outcome.folded) {
            CHECK(SameIndex(reference, index) && waiting == referenceWaiting);
            break;
        }

        if (outcome.folded) {
            // 이벤트 기록 실패 → 전체 다시 검색(덮인 대기는 빠진다).
            CHECK(MatchesRebuild(env, scene, index) && waiting <= referenceWaiting);
            ++folds;
        } else {
            CHECK(SameIndex(reference, index) && waiting == referenceWaiting);
            ++failures;
        }
    }

    CHECK(failures > 0 && folds > 0);

    // 전체 다시 검색(Overflow 빈 경로)의 할당 실패: 색인 그대로, 다음 Pump가 전체를 다시 한다.
    uint32 fullFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex index(failing, scene.layout);
        FakeChangeSource source(env.allocator);
        ManualAssetClock clock(StartTicks);
        AssetIndexWatch watch(failing, index, scene.files, ReadOptions(), source, clock);
        REQUIRE(watch.Start());
        AssetIndex before(env.allocator, scene.layout);
        REQUIRE(Snapshot(scene, before));
        scene.files.Remove("Content/Later.rasset");
        REQUIRE(scene.files.AddFile("Content/Later.rasset", sources.villain.View()));
        REQUIRE(source.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, ""));
        failing.FailOnAttempt(attempt);
        auto pumped = watch.Pump();
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(pumped);
            CHECK(MatchesRebuild(env, scene, index));
            break;
        }

        CHECK(!pumped && pumped.Error() == ErrorCode::OutOfMemory && SameIndex(before, index));
        CHECK(watch.FullRescanPending());
        REQUIRE(watch.Pump());
        CHECK(MatchesRebuild(env, scene, index) && !watch.FullRescanPending());
        scene.files.Remove("Content/Later.rasset");
        ++fullFailures;
    }

    CHECK(fullFailures > 0);
}

// 실제 감시기(PlatformAssetChangeSource): 디스크 프로젝트에서 패키지를 쓰고 지우면 이벤트로 색인이
// 바뀐다. Wait는 이벤트를 기다리는 gate이고 횟수 상한은 실패 판정용이다(시간에 기대지 않는다 —
// 이벤트가 오면 바로 깬다). 시계는 시스템 시계다: 쓰기는 Pump 전에 끝나므로 보통 쓰는 중
// 대기를 거치지 않고, 백신 등이 파일을 잡아 Busy가 나면 실제 500 ms 안정을 기다린 뒤 같은
// 결과에 닿는다(상한 안 — 판정은 같다).
TEST(AssetIndexWatch_RealWatcher) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("watch-real");
    REQUIRE(project.made);
    REQUIRE(MakeContentDirectory(env.allocator, project, "Sub"));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    AssetIndex index(env.allocator, layout);
    PlatformAssetChangeSource source;
    SystemAssetClock clock;
    platform::FileSystem& files = platform::PhysicalFileSystem();
    AssetIndexWatch watch(env.allocator, index, files, ReadOptions(), source, clock);
    REQUIRE(watch.Start());
    CHECK(index.Count() == 0);
    constexpr uint32 MaxRounds = 200;
    constexpr uint32 WaitMilliseconds = 100;
    auto pumpUntil = [&](auto&& done) noexcept {
        for (uint32 round = 0; round < MaxRounds; ++round) {
            (void)source.Wait(WaitMilliseconds);
            if (!watch.Pump()) {
                return false;
            }

            if (done()) {
                return true;
            }
        }

        return false;
    };

    // 없는 디렉터리는 열리지 않는다(NotFound).
    PlatformAssetChangeSource missing;
    String nowhere(env.allocator);
    REQUIRE(nowhere.TryAppend(project.Root()));
    REQUIRE(nowhere.TryAppend("/Content/Nowhere"));
    auto notOpened = missing.Open(nowhere.View());
    CHECK(!notOpened && notOpened.Error() == ErrorCode::NotFound);

    REQUIRE(WriteContentFile(env.allocator, project, "Sub/Hero.rasset", sources.hero.View()));
    CHECK(pumpUntil([&]() noexcept { return EntryOf(index, "Sub/Hero.rasset") != nullptr; }));
    REQUIRE(WriteContentFile(env.allocator, project, "Sub/Hero.rasset", sources.villain.View()));
    CHECK(pumpUntil([&]() noexcept {
        const AssetIndexEntry* hero = EntryOf(index, "Sub/Hero.rasset");
        return hero != nullptr && hero->id == FixedId(VillainId);
    }));
    String path(env.allocator);
    const StringView pieces[] = {project.Root(), "/Content/Sub/Hero.rasset"};
    for (StringView piece : pieces) {
        REQUIRE(path.TryAppend(piece));
    }

    REQUIRE(files.Delete(path.View().Data(), path.View().ByteCount()));
    CHECK(pumpUntil([&]() noexcept { return index.Count() == 0; }));
    watch.Stop();
}

} // namespace asset_index_watch_tests_file
