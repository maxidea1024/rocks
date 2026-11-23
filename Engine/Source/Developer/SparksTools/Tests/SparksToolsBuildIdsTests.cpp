#include <Core/Text/Json.h>
#include <HostConfig/HostSettings.h>
#include <SparksVm/Bytecode.h>
#include <SparksVm/BytecodeFile.h>
#include <TestSupport/NFailAllocator.h>
#include "SparksToolsTestSupport.h"

// `sparks build`·`sparks ids` 시험이다(Docs/Design/SparksTools.ko.md 7.3절 — LC8b): 출력 경로·byte,
// 같은 내용은 다시 쓰지 않음, 원자적 교체, ID 발급·rename의 manifest byte(C# 직렬화와 공용 golden),
// 잠금·동시 변경·교체 실패, 할당 실패 전수.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_tools_build_ids_tests_file {

using namespace rocks::test::sparks_tools;

namespace {

constexpr StringView EngineSchema =
    R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
{ "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object", "scriptExtensible": true, "events": [ { "name": "Destroyed", "id": "5a7e0000000000000000000000000104" } ] },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptableComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true }
] })";

constexpr StringView FullManifest = "{ name: \"Game\", sparks: 1, profile: \"full\" }\n";

// Gate를 Door로(`@formerly("Gate")` — 멤버 승계), Shut을 Open으로 바꾸고 enum case Right와
// handler를 새로 더한 원문이다.
constexpr StringView DoorSource = R"(enum Side { Left, Right }

@formerly("Gate") class Door : ScriptableComponent {
    @save @formerly("Shut") var Open = false;
    var Width = 1;

    on Owner.Destroyed() {
    }
}
)";

// 바꾸기 전 manifest다(항목·aliases 순서를 일부러 섞었다 — 출력은 정규 순서).
constexpr StringView DoorManifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Side", "id": "00000000000000000000000000000004", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Gate.Width", "id": "00000000000000000000000000000003", "aliases": [], "retired": false },
{ "kind": "type", "name": "Game/Doors/Door.Gate", "id": "00000000000000000000000000000001", "aliases": ["Game/Old/Z.Gate", "Game/Doors/Door.Arch"], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Gate.Shut", "id": "00000000000000000000000000000002", "aliases": [], "retired": false },
{ "kind": "enumCase", "name": "Game/Doors/Door.Side.Left", "id": "00000000000000000000000000000005", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Gate.Old", "id": "00000000000000000000000000000006", "aliases": [], "retired": true }
] }
)json";

// golden의 새 ID 자리(실제 발급 값으로 바꿔 대조한다).
constexpr StringView GoldenRightId = "0000000000000000000000000000000a";
constexpr StringView GoldenHandlerId = "0000000000000000000000000000000b";

bool ReadFile(Allocator& allocator, StringView path, String& out) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    Array<char> bytes(allocator);
    const bool ok = file.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                    (size == 0 || file.ReadExact(bytes.Data(), bytes.Size()).status);
    const bool closed = static_cast<bool>(file.Close());
    return ok && closed && out.TryAssign(StringView(bytes.Data(), bytes.Size()));
}

// 이진 파일 전체를 읽는다.
bool ReadBytes(StringView path, Array<uint8>& out) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    const bool ok = file.Size(size) && out.TryResize(static_cast<usize>(size)) &&
                    (size == 0 || file.ReadExact(out.Data(), out.Size()).status);
    const bool closed = static_cast<bool>(file.Close());
    return ok && closed;
}

bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
    bool same = left.Size() == right.Size();
    for (usize index = 0; same && index < left.Size(); ++index) {
        same = left[index] == right[index];
    }

    return same;
}

bool Exists(StringView path) noexcept {
    bool exists = false;
    return platform::PhysicalFileSystem().FileExists(path.Data(), path.ByteCount(), exists) &&
           exists;
}

bool FolderExists(StringView path) noexcept {
    bool exists = false;
    return platform::PhysicalFileSystem().DirectoryExists(path.Data(), path.ByteCount(), exists) &&
           exists;
}

// folder 아래(하위 폴더 포함)에서 이름이 suffix로 끝나는 파일 수다(남은 잠금·임시 파일 검사).
uint32 CountFiles(Allocator& allocator, StringView folder, StringView suffix) noexcept {
    struct Context {
        Allocator* allocator;
        StringView folder;
        StringView suffix;
        uint32 count;
    } context{&allocator, folder, suffix, 0};
    auto visit = [](const platform::DirectoryEntry& entry, void* raw) noexcept {
        auto& state = *static_cast<Context*>(raw);
        const StringView name(entry.name, entry.nameBytes);
        if (entry.info.type == platform::FileType::Directory) {
            auto child = Path::Combine(*state.allocator, state.folder, name);
            CHECK(child);
            if (child) {
                state.count += CountFiles(*state.allocator, child.Value().View(), state.suffix);
            }
        } else if (name.EndsWith(state.suffix, StringComparison::CaseSensitive)) {
            ++state.count;
        }

        return true;
    };
    CHECK(platform::PhysicalFileSystem().EnumerateDirectory(folder.Data(), folder.ByteCount(),
                                                            visit, &context));
    return context.count;
}

// 실제 파일 시스템 앞에 끼우는 시험 wrapper다: 잠금 파일(`*.lock`)을 CreateNew로 만든 직후 hook을
// 부르고(잠금 아래 동시 변경 — 시각에 기대지 않는 gate), Replace를 실패시킬 수 있다.
class HookedFileSystem final : public platform::FileSystem {
public:
    using Hook = void (*)(void* context) noexcept;

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode mode,
                              platform::FileHandle*& out) noexcept override {
        const platform::FileStatus status = inner_.Open(path, pathBytes, mode, out);
        if (status && mode == platform::FileOpenMode::CreateNew && onLock != nullptr &&
            StringView(path, pathBytes).EndsWith(".lock", StringComparison::CaseSensitive)) {
            onLock(hookContext);
        }

        return status;
    }

    platform::FileStatus FileExists(const char* path, usize pathBytes,
                                    bool& exists) noexcept override {
        return inner_.FileExists(path, pathBytes, exists);
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        return inner_.Delete(path, pathBytes);
    }

    platform::FileStatus Stat(const char* path, usize pathBytes,
                              platform::FileInfo& info) noexcept override {
        return inner_.Stat(path, pathBytes, info);
    }

    platform::FileStatus DirectoryExists(const char* path, usize pathBytes,
                                         bool& exists) noexcept override {
        return inner_.DirectoryExists(path, pathBytes, exists);
    }

    platform::FileStatus MakeDirectory(const char* path, usize pathBytes) noexcept override {
        return inner_.MakeDirectory(path, pathBytes);
    }

    platform::FileStatus DeleteDirectory(const char* path, usize pathBytes) noexcept override {
        return inner_.DeleteDirectory(path, pathBytes);
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize pathBytes,
                                            platform::DirectoryVisitor visitor,
                                            void* context) noexcept override {
        return inner_.EnumerateDirectory(path, pathBytes, visitor, context);
    }

    platform::FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner_.Move(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Copy(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner_.Copy(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Replace(const char* from, usize fromBytes, const char* to,
                                 usize toBytes) noexcept override {
        if (failReplace) {
            return platform::FileStatus{platform::FileCode::AccessDenied, 0};
        }

        const platform::FileStatus status = inner_.Replace(from, fromBytes, to, toBytes);
        if (status && notDurableReplace) {
            return platform::FileStatus{platform::FileCode::NotDurable, 0};
        }

        return status;
    }

    platform::FileStatus SetModifiedTime(const char* path, usize pathBytes,
                                         int64 modifiedTicks) noexcept override {
        return inner_.SetModifiedTime(path, pathBytes, modifiedTicks);
    }

    platform::FileStatus SyncDirectory(const char* path, usize pathBytes) noexcept override {
        return inner_.SyncDirectory(path, pathBytes);
    }

    bool failReplace = false;
    bool notDurableReplace = false; // 실제로 교체한 뒤 NotDurable을 돌려준다
    Hook onLock = nullptr;
    void* hookContext = nullptr;

private:
    platform::FileSystem& inner_ = platform::PhysicalFileSystem();
};

// Door 패키지(schema·manifest·원문·ID manifest)를 쓴다.
bool WriteDoorPackage(TempFolder& temp) noexcept {
    return temp.Write("Schema/Engine.sparkbind.json", EngineSchema) &&
           temp.Write("Pkg/SparksPackage.json5", FullManifest) &&
           temp.Write("Pkg/Doors/Door.spark", DoorSource) &&
           temp.Write("Pkg/Game.rocksids.json", DoorManifest);
}

// 수정 시각을 고정 값으로 둔다(다시 쓰지 않았는지 시각 해상도와 무관하게 본다).
constexpr int64 FixedModifiedTicks = 630822816000000000ll; // 2000-01-01 UTC

int64 ModifiedTicks(StringView path) noexcept {
    platform::FileInfo info;
    CHECK(platform::PhysicalFileSystem().Stat(path.Data(), path.ByteCount(), info));
    return info.modifiedTicks;
}

} // namespace

TEST(SparksTools_BuildWritesModules) {
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(temp.Write("Pkg/SparksPackage.json5", Manifest));
        REQUIRE(temp.Write("Pkg/A.spark", "fn F(): int {\n    return 1;\n}\n"));
        REQUIRE(temp.Write("Pkg/Sub/B.spark", "fn G(): int {\n    return 2;\n}\n"));
        const String folder = temp.PathOf("Pkg");
        const String out = temp.PathOf("Out");
        const StringView arguments[] = {"build", "--out", out.View(), folder.View()};
        {
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "0 errors in 2 modules\nwrote 2 of 2 modules\n");
            CHECK(output.error.IsEmpty());
        }

        // `<출력>/<패키지 이름>/<모듈 경로>.sparkc` — 다시 읽혀 verifier를 통과하고, 다시 쓰면 같은
        // byte다(형식 왕복).
        const String a = temp.PathOf("Out/Game/A.sparkc");
        const String b = temp.PathOf("Out/Game/Sub/B.sparkc");
        const StringView imports[] = {"A", "Sub.B"};
        const StringView paths[] = {a.View(), b.View()};
        for (usize at = 0; at < 2; ++at) {
            Array<uint8> bytes(allocator);
            REQUIRE(ReadBytes(paths[at], bytes));
            BytecodeModule module(allocator);
            BytecodeRejection rejection;
            REQUIRE(ReadBytecodeFile(bytes.View(), module, rejection));
            CHECK(module.String(module.path) == imports[at]);
            Array<uint8> again(allocator);
            REQUIRE(WriteBytecodeFile(module, again));
            CHECK(SameBytes(again, bytes));
        }

        CHECK(CountFiles(allocator, out.View(), ".tmp") == 0);

        // 같은 내용은 다시 쓰지 않는다(수정 시각 그대로).
        for (const StringView path : paths) {
            REQUIRE(platform::PhysicalFileSystem().SetModifiedTime(path.Data(), path.ByteCount(),
                                                                   FixedModifiedTicks));
        }

        {
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "0 errors in 2 modules\nwrote 0 of 2 modules\n");
            CHECK(ModifiedTicks(a.View()) == FixedModifiedTicks);
            CHECK(ModifiedTicks(b.View()) == FixedModifiedTicks);
        }

        // 바뀐 모듈만 다시 쓴다.
        REQUIRE(temp.Write("Pkg/Sub/B.spark", "fn G(): int {\n    return 3;\n}\n"));
        {
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "0 errors in 2 modules\nwrote 1 of 2 modules\n");
            CHECK(ModifiedTicks(a.View()) == FixedModifiedTicks);
            CHECK(ModifiedTicks(b.View()) != FixedModifiedTicks);
        }

        // 교체 실패: 3, 임시 파일이 남지 않고 대상은 그대로.
        Array<uint8> before(allocator);
        REQUIRE(ReadBytes(b.View(), before));
        REQUIRE(temp.Write("Pkg/Sub/B.spark", "fn G(): int {\n    return 4;\n}\n"));
        {
            HookedFileSystem files;
            files.failReplace = true;
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, files, arguments, output) == ExitError);
            CHECK(output.error.View().Contains("B.sparkc: cannot write the file",
                                               StringComparison::CaseSensitive));
            CHECK(CountFiles(allocator, out.View(), ".tmp") == 0);
            Array<uint8> after(allocator);
            REQUIRE(ReadBytes(b.View(), after));
            CHECK(SameBytes(after, before));
        }

        // 교체는 됐지만 폴더를 매체에 기록하지 못함(NotDurable): 3, 새 내용은 보이고 임시 파일은
        // 없다.
        REQUIRE(temp.Write("Pkg/Sub/B.spark", "fn G(): int {\n    return 5;\n}\n"));
        {
            HookedFileSystem files;
            files.notDurableReplace = true;
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, files, arguments, output) == ExitError);
            CHECK(output.error.View().Contains("B.sparkc: the file was replaced, but the folder",
                                               StringComparison::CaseSensitive));
            CHECK(CountFiles(allocator, out.View(), ".tmp") == 0);
            Array<uint8> after(allocator);
            REQUIRE(ReadBytes(b.View(), after));
            CHECK(!SameBytes(after, before));
            BufferOutput again(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, again) ==
                  ExitSuccess);
            CHECK(again.output.View().EndsWith("wrote 0 of 2 modules\n"));
        }

        // 진단이 있으면 아무것도 쓰지 않는다(출력 폴더도 만들지 않는다).
        REQUIRE(temp.Write("Pkg/A.spark", "fn F(): int {\n    return true;\n}\n"));
        {
            const String other = temp.PathOf("Other");
            const StringView failing[] = {"build", "--out", other.View(), folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), failing, output) ==
                  ExitFailure);
            CHECK(output.output.View().EndsWith("1 errors in 2 modules\n"));
            CHECK(!FolderExists(other.View()));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_IdsIssuesAndRenames) {
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(WriteDoorPackage(temp));
        const String schema = temp.PathOf("Schema");
        const String folder = temp.PathOf("Pkg");
        const String manifest = temp.PathOf("Pkg/Game.rocksids.json");
        const String shown = Slashed(allocator, folder.View());
        const StringView arguments[] = {"ids", "--schema", schema.View(), folder.View()};
        {
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            StringBuilder<512> expected(allocator);
            REQUIRE(expected.TryAppendFormat(
                "0 errors in 1 modules\n{}/Game.rocksids.json: 2 new, 2 renamed\n", shown.View()));
            CHECK(output.output.View() == expected.View());
            CHECK(output.error.IsEmpty());
        }

        // 결과 byte: golden(C# 직렬화 시험과 공용)의 새 ID 두 자리만 실제 발급 값으로 바꾼 것과
        // 같다.
        String written(allocator);
        REQUIRE(ReadFile(allocator, manifest.View(), written));
        JsonDocument document(allocator);
        REQUIRE(document.Parse(written.View()).Succeeded());
        StringView right;
        StringView handler;
        const JsonValue entries = document.Root().Find("entries");
        for (usize index = 0; index < entries.Size(); ++index) {
            const StringView name = entries.At(index).Find("name").GetString();
            const StringView id = entries.At(index).Find("id").GetString();
            if (name == "Game/Doors/Door.Side.Right") {
                right = id;
            } else if (name == "Game/Doors/Door.Door.on(Owner.Destroyed)") {
                handler = id;
            }
        }

        const StringView issued[] = {right, handler};
        for (const StringView id : issued) {
            bool hex = id.ByteCount() == 32;
            bool nonzero = false;
            for (usize at = 0; hex && at < id.ByteCount(); ++at) {
                const char c = id.Data()[at];
                hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                nonzero = nonzero || c != '0';
            }

            CHECK(hex && nonzero);
        }

        CHECK(right != handler);
        constexpr usize PathCapacity = 4096;
        char engine[PathCapacity];
        auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
        REQUIRE(found);
        auto goldenPath =
            Path::Combine(allocator, StringView(engine, found.Value()),
                          "Source/Developer/SparksTools/Tests/Data/IdsGolden.rocksids.json");
        REQUIRE(goldenPath);
        String golden(allocator);
        REQUIRE(ReadFile(allocator, goldenPath.Value().View(), golden));
        String expected(allocator);
        for (usize at = 0; at < golden.ByteCount();) {
            const StringView rest(golden.Data() + at, golden.ByteCount() - at);
            if (rest.StartsWith(GoldenRightId) || rest.StartsWith(GoldenHandlerId)) {
                REQUIRE(expected.TryAppend(rest.StartsWith(GoldenRightId) ? right : handler));
                at += GoldenRightId.ByteCount();
                continue;
            }

            REQUIRE(expected.TryAppend(StringView(rest.Data(), 1)));
            ++at;
        }

        CHECK(written.View() == expected.View());
        CHECK(CountFiles(allocator, folder.View(), ".lock") == 0);
        CHECK(CountFiles(allocator, folder.View(), ".tmp") == 0);

        // 다시 돌리면 대기가 없다(파일 그대로), check는 진단 없음.
        {
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "0 errors in 1 modules\nno pending IDs\n");
            String again(allocator);
            REQUIRE(ReadFile(allocator, manifest.View(), again));
            CHECK(again.View() == written.View());
            const StringView check[] = {"check", "--schema", schema.View(), folder.View()};
            BufferOutput checked(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), check, checked) ==
                  ExitSuccess);
        }

        // manifest 없는 패키지: 뿌리에 `<패키지 이름>.rocksids.json`을 새로 만든다.
        REQUIRE(temp.Write("Fresh/SparksPackage.json5",
                           "{ name: \"Fresh\", sparks: 1, profile: \"core\" }\n"));
        REQUIRE(temp.Write("Fresh/Stats.spark", "struct Stats {\n    var Level: int = 1;\n}\n"));
        const String fresh = temp.PathOf("Fresh");
        {
            const StringView freshArguments[] = {"ids", fresh.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), freshArguments, output) ==
                  ExitSuccess);
            StringBuilder<512> expected(allocator);
            REQUIRE(expected.TryAppendFormat(
                "0 errors in 1 modules\n{}/Fresh.rocksids.json: 2 new, 0 renamed\n",
                Slashed(allocator, fresh.View()).View()));
            CHECK(output.output.View() == expected.View());
            CHECK(Exists(temp.PathOf("Fresh/Fresh.rocksids.json").View()));
            BufferOutput again(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), freshArguments, again) ==
                  ExitSuccess);
            CHECK(again.output.View().EndsWith("no pending IDs\n"));
        }

        // 진단이 있으면 발급하지 않는다(manifest를 만들지 않는다).
        REQUIRE(temp.Write("Bad/SparksPackage.json5",
                           "{ name: \"Bad\", sparks: 1, profile: \"core\" }\n"));
        REQUIRE(temp.Write("Bad/Stats.spark", "struct Stats {\n    var Level: int = true;\n}\n"));
        {
            const String bad = temp.PathOf("Bad");
            const StringView badArguments[] = {"ids", bad.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), badArguments, output) ==
                  ExitFailure);
            CHECK(output.output.View().EndsWith("1 errors in 1 modules\n"));
            CHECK(!Exists(temp.PathOf("Bad/Bad.rocksids.json").View()));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_IdsSeveralPackages) {
    // 패키지 둘(기존 manifest Game, manifest 없는 GameX — 이름 앞부분이 같다)을 한 번에: 대기는
    // 이름 키의 패키지로 정확히 나뉘고, 결과 줄은 인자 순서와 무관하게 manifest 진단 경로 byte
    // 순이다.
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(WriteDoorPackage(temp));
        REQUIRE(temp.Write("X/SparksPackage.json5",
                           "{ name: \"GameX\", sparks: 1, profile: \"core\" }\n"));
        REQUIRE(temp.Write("X/Stats.spark", "struct Stats {\n    var Level: int = 1;\n}\n"));
        const String schema = temp.PathOf("Schema");
        const String game = temp.PathOf("Pkg");
        const String gameX = temp.PathOf("X");
        const StringView arguments[] = {"ids", "--schema", schema.View(), gameX.View(),
                                        game.View()};
        BufferOutput output(allocator);
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitSuccess);
        StringBuilder<1024> expected(allocator);
        REQUIRE(expected.TryAppendFormat("0 errors in 2 modules\n"
                                         "{}/Game.rocksids.json: 2 new, 2 renamed\n"
                                         "{}/GameX.rocksids.json: 2 new, 0 renamed\n",
                                         Slashed(allocator, game.View()).View(),
                                         Slashed(allocator, gameX.View()).View()));
        CHECK(output.output.View() == expected.View());
        String first(allocator);
        String second(allocator);
        REQUIRE(ReadFile(allocator, temp.PathOf("Pkg/Game.rocksids.json").View(), first));
        REQUIRE(ReadFile(allocator, temp.PathOf("X/GameX.rocksids.json").View(), second));
        CHECK(!first.View().Contains("GameX/", StringComparison::CaseSensitive));
        CHECK(second.View().Contains("\"name\": \"GameX/Stats.Stats\"",
                                     StringComparison::CaseSensitive));
        CHECK(second.View().Contains("\"name\": \"GameX/Stats.Stats.Level\"",
                                     StringComparison::CaseSensitive));
        BufferOutput again(allocator);
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, again) ==
              ExitSuccess);
        CHECK(again.output.View() == "0 errors in 2 modules\nno pending IDs\n");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_IdsLockAndConcurrentChange) {
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(WriteDoorPackage(temp));
        const String schema = temp.PathOf("Schema");
        const String folder = temp.PathOf("Pkg");
        const String manifest = temp.PathOf("Pkg/Game.rocksids.json");
        const String lock = temp.PathOf("Pkg/Game.rocksids.json.lock");
        const StringView arguments[] = {"ids", "--schema", schema.View(), folder.View()};

        // 잠금 파일이 있으면 1(다른 도구가 쓰는 중 — 파일 그대로, 잠금은 지우지 않는다).
        REQUIRE(temp.Write("Pkg/Game.rocksids.json.lock", "held"));
        {
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitFailure);
            CHECK(output.error.View().Contains("is locked by another tool",
                                               StringComparison::CaseSensitive));
            String after(allocator);
            REQUIRE(ReadFile(allocator, manifest.View(), after));
            CHECK(after.View() == DoorManifest);
            CHECK(Exists(lock.View()));
        }

        REQUIRE(platform::PhysicalFileSystem().Delete(lock.Data(), lock.ByteCount()));

        // 잠금을 잡은 직후 manifest가 바뀌면 1(쓰지 않는다, 잠금은 지운다).
        struct Tamper {
            TempFolder* temp;
            StringView relative;
            StringView content;
        };
        constexpr StringView Changed = "{ \"schema\": 1, \"entries\": [] }\n";
        Tamper tamper{&temp, "Pkg/Game.rocksids.json", Changed};
        auto rewrite = [](void* raw) noexcept {
            auto& state = *static_cast<Tamper*>(raw);
            CHECK(state.temp->Write(state.relative, state.content));
        };
        {
            HookedFileSystem files;
            files.onLock = rewrite;
            files.hookContext = &tamper;
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, files, arguments, output) == ExitFailure);
            CHECK(output.error.View().Contains("the manifest changed while the IDs were issued",
                                               StringComparison::CaseSensitive));
            String after(allocator);
            REQUIRE(ReadFile(allocator, manifest.View(), after));
            CHECK(after.View() == Changed);
            CHECK(!Exists(lock.View()));
        }

        // 새 manifest인데 잠금 아래에서 누가 만들었다: 1.
        REQUIRE(temp.Write("Fresh/SparksPackage.json5",
                           "{ name: \"Fresh\", sparks: 1, profile: \"core\" }\n"));
        REQUIRE(temp.Write("Fresh/Stats.spark", "struct Stats {\n    var Level: int = 1;\n}\n"));
        const String fresh = temp.PathOf("Fresh");
        {
            Tamper created{&temp, "Fresh/Fresh.rocksids.json", Changed};
            HookedFileSystem files;
            files.onLock = rewrite;
            files.hookContext = &created;
            const StringView freshArguments[] = {"ids", fresh.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, files, freshArguments, output) == ExitFailure);
            CHECK(output.error.View().Contains("the manifest changed while the IDs were issued",
                                               StringComparison::CaseSensitive));
            CHECK(!Exists(temp.PathOf("Fresh/Fresh.rocksids.json.lock").View()));
        }

        // 교체 실패: 3, 잠금·임시 파일이 남지 않고 원본 그대로.
        REQUIRE(temp.Write("Pkg/Game.rocksids.json", DoorManifest));
        {
            HookedFileSystem files;
            files.failReplace = true;
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, files, arguments, output) == ExitError);
            CHECK(output.error.View().Contains("Game.rocksids.json: cannot write the file",
                                               StringComparison::CaseSensitive));
            String after(allocator);
            REQUIRE(ReadFile(allocator, manifest.View(), after));
            CHECK(after.View() == DoorManifest);
            CHECK(CountFiles(allocator, folder.View(), ".lock") == 0);
            CHECK(CountFiles(allocator, folder.View(), ".tmp") == 0);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_BuildIdsAllocationFailure) {
    // build·ids의 모든 할당 실패 자리: ExitError로 끝나고 새는 할당·남는 잠금·임시 파일이 없다.
    // 시도마다 출력 폴더를 지우고 manifest를 되돌린다(같은 입력).
    SystemAllocator backing;
    TempFolder temp(backing);
    REQUIRE(WriteDoorPackage(temp));
    const String schema = temp.PathOf("Schema");
    const String folder = temp.PathOf("Pkg");
    const String out = temp.PathOf("Out");
    const StringView build[] = {"build", "--schema", schema.View(),
                                "--out", out.View(), folder.View()};
    const StringView ids[] = {"ids", "--schema", schema.View(), folder.View()};
    const Span<const StringView> commands[] = {build, ids};
    for (const Span<const StringView> arguments : commands) {
        uint64 attempts = 0;
        uint64 failures = 0;
        for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
            REQUIRE(temp.Write("Pkg/Game.rocksids.json", DoorManifest));
            if (FolderExists(out.View())) {
                REQUIRE(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), out.Data(),
                                                      out.ByteCount()));
            }

            test::NFailAllocator allocator(backing);
            const uint64 before = backing.Stats().liveAllocations;
            {
                BufferOutput output(backing); // 출력은 세는 allocator 밖에 둔다
                if (failAt != 0) {
                    allocator.FailOnAttempt(failAt);
                }

                const int32 code =
                    RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output);
                const uint64 used = allocator.Attempts();
                allocator.DisableFailure();
                if (failAt == 0) {
                    attempts = used;
                    CHECK(code == ExitSuccess);
                } else if (code == ExitError) {
                    ++failures;
                } else {
                    CHECK(code == ExitSuccess);
                }
            }

            CHECK(backing.Stats().liveAllocations == before);
            CHECK(CountFiles(backing, folder.View(), ".lock") == 0);
            CHECK(CountFiles(backing, folder.View(), ".tmp") == 0);
            if (FolderExists(out.View())) {
                CHECK(CountFiles(backing, out.View(), ".tmp") == 0);
            }
        }

        CHECK(failures > 0);
    }
}

} // namespace sparks_tools_build_ids_tests_file
