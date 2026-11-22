#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetDatabaseTestSupport.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetImporter.h>
#include <AssetDatabase/AssetSourceRoots.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetTestFixtures/TestAssets.generated.h>
#include <Core/Text/Path.h>
#include <TestSupport/NFailAllocator.h>

// 임포트 기반 시험이다(Docs/Design/AssetSystem.ko.md 4.5절, 7.1절 18 A2f1a): 원본 루트 표(없음·
// 기본 Project·덮어쓰기·상대·절대·`..`·정렬, 칸마다 위반·JSON 문법·크기·수 상한·IO·할당 실패), 기록
// 위치(가장 긴 접두·성분 경계·대소문자 무시·같은 경로 이름 순·루트 자신·밖의 절대 경로·상대 거부·
// 길이 상한), 풀기(루트·절대·모르는 루트·규칙 위반), 원본 읽기(내용·SHA-256·없음·상한·IO·할당
// 실패), ImportRecord 원본 위치 규칙(빈 루트의 절대 경로 셋), 임포터 표(등록·찾기·중복·규칙 위반·
// 옵션 이름 충돌·해제·할당 실패).
using namespace rocks;
namespace asset_import_foundation_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;
using asset_fixture::CollidingImportOptions;
using asset_fixture::ReservedImportOptions;
using asset_fixture::TestImportOptions;

namespace {

constexpr StringView SourcesFile = "Config/AssetSources.json";

// 메모리 프로젝트(루트 MemoryRoot — Config·Content)다.
struct RootsScene {
    explicit RootsScene(Allocator& allocator) noexcept
        : layout(allocator), files(allocator, MemoryRoot), roots(allocator, layout) {
        ready = static_cast<bool>(layout.Initialize(MemoryRoot)) && files.Ready() &&
                files.AddDirectory("Config") && files.AddDirectory("Content");
    }

    bool Put(StringView text) noexcept {
        files.Remove(SourcesFile);
        return files.AddFile(
            SourcesFile,
            Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
    }

    ProjectLayout layout;
    MemoryFileSystem files;
    AssetSourceRoots roots;
    bool ready = false;
};

// MemoryRoot 아래 경로(`/` 구분)를 out에 쓴다.
String Under(Allocator& allocator, StringView relative) noexcept {
    String out(allocator);
    CHECK(out.TryAppend(MemoryRoot) && out.TryAppend("/") && out.TryAppend(relative));
    return out;
}

// 어느 루트 아래도 아닌 절대 경로다(호스트 문법).
constexpr StringView OutsidePath = IsWindows ? "D:/Outside/Rock.png" : "/outside/Rock.png";

// 시험 임포터다: info를 그대로 돌려주고 Import는 부르지 않는다(A2f1a는 표만 본다).
class InfoImporter final : public AssetImporter {
public:
    explicit InfoImporter(const AssetImporterInfo& info) noexcept : info_(info) {
    }

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    Result<void> Import(AssetImportContext&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    AssetImporterInfo info_;
};

constexpr StringView PngExtensions[] = {"png", "tga"};
constexpr StringView CubeRoles[] = {"px", "nx", "0.py"};

AssetImporterInfo ValidInfo() noexcept {
    AssetImporterInfo info;
    info.id = Guid{0x494d50, 1};
    info.name = "Test_Importer2";
    info.version = 2;
    info.outputType = IdOf<TestAsset>();
    info.extensions = PngExtensions;
    info.optionsType = GeneratedType<TestImportOptions>::Id();
    return info;
}

} // namespace

// 루트 표 읽기: 파일 없음(Project = 프로젝트 루트 하나), 상대·`..`·절대·OS 구분자 경로의 정규화,
// Project 덮어쓰기, 이름 byte 순 정렬, 다시 읽으면 바꿈.
TEST(AssetSourceRoots_Load) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RootsScene scene(env.allocator);
    REQUIRE(scene.ready);
    REQUIRE(scene.roots.Load(scene.files));
    REQUIRE(scene.roots.Roots().Size() == 1);
    CHECK(scene.roots.Roots()[0].name.View() == ProjectSourceRootName &&
          scene.roots.Roots()[0].path.View() == StringView(MemoryRoot) &&
          scene.roots.Roots()[0].components == (IsWindows ? 2u : 1u));

    // 상대·`..`·절대 루트(정렬 — 이름 byte 순), Project 없음이면 더한다.
    String outside(env.allocator);
    REQUIRE(outside.TryAppend(IsWindows ? "E:\\\\Shared\\\\Art" : "/shared/art"));
    String text(env.allocator);
    REQUIRE(text.TryAppend("{\"roots\": {\"Zeta\": \"SourceArt/./Deep/\", \"Up\": \"../Shared\", "
                           "\"Abs\": \"") &&
            text.TryAppend(outside.View()) && text.TryAppend("\"}}"));
    REQUIRE(scene.Put(text.View()));
    REQUIRE(scene.roots.Load(scene.files));
    REQUIRE(scene.roots.Roots().Size() == 4);
    CHECK(scene.roots.Roots()[0].name.View() == "Abs" &&
          scene.roots.Roots()[1].name.View() == "Project" &&
          scene.roots.Roots()[2].name.View() == "Up" &&
          scene.roots.Roots()[3].name.View() == "Zeta");
    CHECK(scene.roots.Roots()[0].path.View() ==
          (IsWindows ? StringView("E:/Shared/Art") : StringView("/shared/art")));
    const String deep = Under(env.allocator, "SourceArt/Deep");
    CHECK(scene.roots.Roots()[3].path.View() == deep.View() &&
          scene.roots.Roots()[3].components == (IsWindows ? 4u : 3u));
    CHECK(scene.roots.Roots()[2].path.View() ==
          (IsWindows ? StringView("C:/Shared") : StringView("/Shared")));

    // Project 덮어쓰기.
    REQUIRE(scene.Put("{\"roots\": {\"Project\": \"Other\"}}"));
    REQUIRE(scene.roots.Load(scene.files));
    REQUIRE(scene.roots.Roots().Size() == 1);
    const String other = Under(env.allocator, "Other");
    CHECK(scene.roots.Roots()[0].path.View() == other.View());
    // 빈 roots.
    REQUIRE(scene.Put("{\"roots\": {}}"));
    REQUIRE(scene.roots.Load(scene.files));
    CHECK(scene.roots.Roots().Size() == 1);
}

// 신뢰하지 않는 루트 표: 칸마다 위반은 InvalidArgument / InvalidSource(detail — JSON 위치·항목
// 번호 + 1), 크기·수 상한 CapacityExceeded, IO 오류(진단 None), 실패하면 빈 표, 할당 실패.
TEST(AssetSourceRoots_RejectsUntrustedInput) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RootsScene scene(env.allocator);
    REQUIRE(scene.ready);
    // JsonPosition은 이 값이면 detail의 줄(위 32 bit)이 1이고 열이 0이 아닌지만 본다.
    constexpr uint64 JsonLineOne = ~uint64{0};
    auto rejects = [&](StringView text, ErrorCode error, uint64 detail) noexcept {
        if (!scene.Put(text)) {
            return false;
        }

        AssetDiagnostic failure;
        auto loaded = scene.roots.Load(scene.files, &failure);
        const bool located = detail == JsonLineOne
                                 ? (failure.detail >> 32) == 1 && (failure.detail & 0xffffffff) != 0
                                 : failure.detail == detail;
        return !loaded && loaded.Error() == error &&
               failure.code == AssetDiagnosticCode::InvalidSource && located &&
               scene.roots.Roots().IsEmpty();
    };
    CHECK(rejects("{", ErrorCode::InvalidArgument, JsonLineOne));
    CHECK(rejects("[]", ErrorCode::InvalidArgument, 0));
    CHECK(rejects("{}", ErrorCode::InvalidArgument, 0));
    CHECK(rejects("{\"roots\": {}, \"extra\": 1}", ErrorCode::InvalidArgument, 0));
    CHECK(rejects("{\"other\": {}}", ErrorCode::InvalidArgument, 0));
    CHECK(rejects("{\"roots\": []}", ErrorCode::InvalidArgument, 0));
    CHECK(rejects("{\"roots\": {\"A\": 1}}", ErrorCode::InvalidArgument, 1));
    CHECK(rejects("{\"roots\": {\"A\": \"\"}}", ErrorCode::InvalidArgument, 1));
    CHECK(rejects("{\"roots\": {\"A\": \"x\", \"b-c\": \"y\"}}", ErrorCode::InvalidArgument, 2));
    CHECK(rejects("{\"roots\": {\"\": \"y\"}}", ErrorCode::InvalidArgument, 1));
    const StringView badRootNames[] = {"a{", "a:", "a[", "a`", "a@", "a/"};
    for (StringView name : badRootNames) {
        String text(env.allocator);
        REQUIRE(text.TryAppend("{\"roots\": {\"") && text.TryAppend(name) &&
                text.TryAppend("\": \"y\"}}"));
        CHECK(rejects(text.View(), ErrorCode::InvalidArgument, 1));
    }

    // 대소문자만 다른 두 이름(기본 Project와 겹치는 project 포함 — 더한 Project가 마지막 번호).
    CHECK(rejects("{\"roots\": {\"Art\": \"x\", \"ART\": \"y\"}}", ErrorCode::InvalidArgument, 2));
    CHECK(rejects("{\"roots\": {\"project\": \"x\"}}", ErrorCode::InvalidArgument, 2));
    REQUIRE(scene.Put("{\"roots\": {\"My_Root9\": \"y\"}}"));
    CHECK(scene.roots.Load(scene.files) && scene.roots.Roots().Size() == 2);
    CHECK(rejects("{\"roots\": {\"A\": \"x\", \"A\": \"y\"}}", ErrorCode::InvalidArgument,
                  JsonLineOne));
    // 이름 256 byte는 받고 257 byte는 거부한다.
    {
        String accepted(env.allocator);
        String rejected(env.allocator);
        REQUIRE(accepted.TryAppend("{\"roots\": {\"") && rejected.TryAppend("{\"roots\": {\"a"));
        for (usize index = 0; index < MaxImportRootBytes; ++index) {
            REQUIRE(accepted.TryAppend("a") && rejected.TryAppend("a"));
        }

        REQUIRE(accepted.TryAppend("\": \"x\"}}") && rejected.TryAppend("\": \"x\"}}"));
        REQUIRE(scene.Put(accepted.View()));
        CHECK(scene.roots.Load(scene.files) && scene.roots.Roots().Size() == 2);
        CHECK(rejects(rejected.View(), ErrorCode::InvalidArgument, 1));
    }

    // 드라이브 상대·현재 드라이브 루트(Windows만 — POSIX에서는 상대 경로다), 장치 이름공간(`\\?\` —
    // Core Path가 Unsupported로 거부한다, UNC로 잘못 기록하지 않는다).
    if (IsWindows) {
        // JSON 안에서 `\\`가 `\` 하나다 → 값은 `\\?\C:\x`.
        CHECK(rejects("{\"roots\": {\"A\": \"\\\\\\\\?\\\\C:\\\\x\"}}", ErrorCode::Unsupported, 1));
        CHECK(rejects("{\"roots\": {\"A\": \"C:x\"}}", ErrorCode::InvalidArgument, 1));
        CHECK(rejects("{\"roots\": {\"A\": \"\\\\x\"}}", ErrorCode::InvalidArgument, 1));
    }

    // 루트 위로 벗어나는 `..`.
    CHECK(
        rejects(IsWindows ? "{\"roots\": {\"A\": \"C:/../x\"}}" : "{\"roots\": {\"A\": \"/../x\"}}",
                ErrorCode::InvalidArgument, 1));
    // 루트 수 상한(256 받음, 257 거부).
    {
        String full(env.allocator);
        REQUIRE(full.TryAppend("{\"roots\": {"));
        for (usize index = 0; index <= MaxAssetSourceRoots; ++index) {
            char name[16] = {};
            FixedStringBuilder entry{Span<char>(name)};
            REQUIRE(entry.TryAppendFormat("r{}", index));
            if (index == MaxAssetSourceRoots) {
                String accepted(env.allocator);
                REQUIRE(accepted.TryAppend(full.View()) && accepted.TryAppend("}}"));
                REQUIRE(scene.Put(accepted.View()));
                CHECK(scene.roots.Load(scene.files) &&
                      scene.roots.Roots().Size() == MaxAssetSourceRoots + 1);
            }

            REQUIRE(full.TryAppend(index == 0 ? "\"" : ", \"") && full.TryAppend(entry.View()) &&
                    full.TryAppend("\": \"x\""));
        }

        REQUIRE(full.TryAppend("}}"));
        CHECK(rejects(full.View(), ErrorCode::CapacityExceeded, JsonLineOne));
    }

    // 파일 크기 상한.
    {
        Array<uint8> large(env.allocator);
        REQUIRE(large.TryResize(static_cast<usize>(MaxAssetSourceRootsFileBytes) + 1));
        for (uint8& value : large) {
            value = ' ';
        }

        scene.files.Remove(SourcesFile);
        REQUIRE(scene.files.AddFile(SourcesFile, large.View()));
        AssetDiagnostic failure;
        auto loaded = scene.roots.Load(scene.files, &failure);
        CHECK(!loaded && loaded.Error() == ErrorCode::CapacityExceeded &&
              failure.code == AssetDiagnosticCode::InvalidSource);
    }

    // 읽기 IO 오류(진단 None — 그 오류).
    REQUIRE(scene.Put("{\"roots\": {}}"));
    scene.files.faults.openRead = MemoryBusy;
    scene.files.faults.readSuffix = "AssetSources.json";
    AssetDiagnostic failure;
    auto busy = scene.roots.Load(scene.files, &failure);
    scene.files.faults.openRead = {};
    scene.files.faults.readSuffix = nullptr;
    CHECK(!busy && busy.Error() == ErrorCode::Busy && failure.code == AssetDiagnosticCode::None);
    // 실패 진단 없이도.
    REQUIRE(scene.Put("{"));
    CHECK(!scene.roots.Load(scene.files));

    // 할당 실패: 모두 OutOfMemory(진단 None)이고 빈 표.
    REQUIRE(scene.Put("{\"roots\": {\"Zeta\": \"SourceArt\", \"Abs\": \"Elsewhere/x\"}}"));
    test::NFailAllocator failing(env.allocator);
    AssetSourceRoots roots(failing, scene.layout);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        AssetDiagnostic oom;
        auto loaded = roots.Load(scene.files, &oom);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(loaded);
            CHECK(roots.Roots().Size() == 3);
            break;
        }

        CHECK(!loaded && loaded.Error() == ErrorCode::OutOfMemory &&
              oom.code == AssetDiagnosticCode::None && roots.Roots().IsEmpty());
        ++failures;
    }

    CHECK(failures > 0);
}

// 기록 위치: 성분 단위 가장 긴 접두 루트, 성분 경계(SourceArtX는 SourceArt 아래가 아님), 대소문자
// 무시(relative는 원래 철자), 같은 경로 두 이름은 이름 순 첫째, 루트 자신은 그 위 루트 아래, 어느
// 루트 아래도 아니면 절대 경로(absolute), `.`·`..` 정규화, 상대·드라이브 상대 거부, 길이 상한.
// 풀기: 루트·절대·드라이브 루트(끝 `/`), 모르는 루트 NotFound, 규칙 위반 InvalidArgument.
TEST(AssetSourceRoots_LocateAndResolve) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RootsScene scene(env.allocator);
    REQUIRE(scene.ready);
    String text(env.allocator);
    REQUIRE(text.TryAppend("{\"roots\": {\"Art\": \"SourceArt\", \"Deep\": \"SourceArt/Deep\", "
                           "\"Twin\": \"SourceArt\", \"Drive\": \"") &&
            text.TryAppend(IsWindows ? "F:/" : "/") && text.TryAppend("\"}}"));
    REQUIRE(scene.Put(text.View()));
    REQUIRE(scene.roots.Load(scene.files));
    AssetSourceLocation location(env.allocator);
    auto locates = [&](StringView relative, StringView root, StringView expected) noexcept {
        const String path = Under(env.allocator, relative);
        return scene.roots.Locate(path.View(), location) && location.root.View() == root &&
               location.relative.View() == expected && !location.absolute;
    };
    CHECK(locates("SourceArt/Deep/Rock.png", "Deep", "Rock.png"));
    CHECK(locates("SourceArt/a/b.png", "Art", "a/b.png"));
    CHECK(locates("SourceArtX/c.png", "Project", "SourceArtX/c.png"));
    CHECK(locates("sourceart/DEEP/Rock.png", "Deep", "Rock.png"));
    CHECK(locates("SourceArt/./x/../Rock.png", "Art", "Rock.png"));
    CHECK(locates("SourceArt", "Project", "SourceArt"));
    CHECK(locates("Content/Raw/Hero.png", "Project", "Content/Raw/Hero.png"));
    // 드라이브 루트(`/` 또는 `F:/` — 끝 `/`) 아래 다른 경로는 Drive.
    {
        const StringView elsewhere = IsWindows ? "F:/Elsewhere/x.png" : "/Elsewhere/x.png";
        REQUIRE(scene.roots.Locate(elsewhere, location));
        CHECK(location.root.View() == "Drive" && location.relative.View() == "Elsewhere/x.png");
    }

    // Drive를 빼면 밖의 절대 경로.
    REQUIRE(scene.Put("{\"roots\": {\"Art\": \"SourceArt\"}}"));
    REQUIRE(scene.roots.Load(scene.files));
    // 연속 `/`로 시작하는 루트(UNC — POSIX는 `/`로 축약된다) 아래.
    REQUIRE(scene.Put("{\"roots\": {\"Net\": \"//server/share\"}}"));
    REQUIRE(scene.roots.Load(scene.files));
    const StringView netPath =
        IsWindows ? StringView("//server/share/a/b.png") : StringView("/server/share/a/b.png");
    REQUIRE(scene.roots.Locate(netPath, location));
    CHECK(location.root.View() == "Net" && location.relative.View() == "a/b.png");
    REQUIRE(scene.Put("{\"roots\": {\"Art\": \"SourceArt\"}}"));
    REQUIRE(scene.roots.Load(scene.files));
    REQUIRE(scene.roots.Locate(OutsidePath, location));
    CHECK(location.root.IsEmpty() && location.relative.View() == OutsidePath && location.absolute);
    if (IsWindows) {
        REQUIRE(scene.roots.Locate("\\\\server\\share\\a\\b.png", location));
        CHECK(location.absolute && location.relative.View() == "//server/share/a/b.png");
        CHECK(scene.roots.Locate("C:x.png", location).Error() == ErrorCode::InvalidArgument);
        CHECK(scene.roots.Locate("\\\\?\\C:\\a.png", location).Error() == ErrorCode::Unsupported);
        CHECK(scene.roots.Locate("\\\\?\\UNC\\server\\share\\a.png", location).Error() ==
              ErrorCode::Unsupported);
        CHECK(scene.roots.Locate("\\x.png", location).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(scene.roots.Locate("relative/x.png", location).Error() == ErrorCode::InvalidArgument);
    // 경로 해석 오류(NUL)와 루트뿐인 경로(기록 규칙이 성분을 요구한다).
    CHECK(scene.roots.Locate(StringView("/a\0b", 4), location).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(scene.roots.Locate(IsWindows ? "F:/" : "/", location).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(scene.roots.Locate("", location).Error() == ErrorCode::InvalidArgument);
    // 기록 경로 길이 상한(1024 byte 받음, 1025 거부).
    {
        String accepted(env.allocator);
        REQUIRE(accepted.TryAppend("SourceArt/"));
        while (accepted.ByteCount() - StringView("SourceArt/").ByteCount() <
               MaxImportRelativePathBytes) {
            REQUIRE(accepted.TryAppend("a"));
        }

        const String path = Under(env.allocator, accepted.View());
        REQUIRE(scene.roots.Locate(path.View(), location));
        CHECK(location.relative.ByteCount() == MaxImportRelativePathBytes);
        REQUIRE(accepted.TryAppend("a"));
        const String longer = Under(env.allocator, accepted.View());
        CHECK(scene.roots.Locate(longer.View(), location).Error() == ErrorCode::CapacityExceeded);
    }

    // 풀기.
    String resolved(env.allocator);
    REQUIRE(scene.roots.Resolve("Art", "a/b.png", resolved));
    const String art = Under(env.allocator, "SourceArt/a/b.png");
    CHECK(resolved.View() == art.View());
    REQUIRE(scene.roots.Resolve("", OutsidePath, resolved));
    CHECK(resolved.View() == OutsidePath);
    CHECK(scene.roots.Resolve("Nope", "x.png", resolved).Error() == ErrorCode::NotFound);
    CHECK(scene.roots.Resolve("Art", "../x.png", resolved).Error() == ErrorCode::InvalidArgument);
    CHECK(scene.roots.Resolve("", "x.png", resolved).Error() == ErrorCode::InvalidArgument);
    CHECK(scene.roots.Resolve("A/B", "x.png", resolved).Error() == ErrorCode::InvalidArgument);
    // 드라이브 루트는 `/`를 겹치지 않는다.
    String drive(env.allocator);
    REQUIRE(drive.TryAppend("{\"roots\": {\"Drive\": \"") &&
            drive.TryAppend(IsWindows ? "F:/" : "/") && drive.TryAppend("\"}}"));
    REQUIRE(scene.Put(drive.View()));
    REQUIRE(scene.roots.Load(scene.files));
    REQUIRE(scene.roots.Resolve("Drive", "a.png", resolved));
    CHECK(resolved.View() == (IsWindows ? StringView("F:/a.png") : StringView("/a.png")));

    // 할당 실패(Locate·Resolve): OutOfMemory.
    test::NFailAllocator failing(env.allocator);
    AssetSourceRoots roots(failing, scene.layout);
    REQUIRE(scene.Put("{\"roots\": {\"Art\": \"SourceArt\"}}"));
    REQUIRE(roots.Load(scene.files));
    const String source = Under(env.allocator, "SourceArt/x/y.png");
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetSourceLocation found(failing);
        String out(failing);
        failing.FailOnAttempt(attempt);
        Result<void> step = roots.Locate(source.View(), found);
        if (step) {
            step = roots.Resolve(found.root.View(), found.relative.View(), out);
        }

        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(step);
            CHECK(out.View() == source.View());
            break;
        }

        CHECK(!step && step.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

// 원본 읽기: 내용과 SHA-256, 없음 MissingSource / NotFound, 상한 InvalidSource / CapacityExceeded
// (상한과 같은 크기는 받는다), 읽기 IO 오류 InvalidSource / 그 오류, 할당 실패(진단 None), 빈 파일.
TEST(AssetImportSource_Read) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RootsScene scene(env.allocator);
    REQUIRE(scene.ready);
    const uint8 content[] = {'r', 'o', 'c', 'k'};
    REQUIRE(scene.files.AddFile("Content/Rock.png", Span<const uint8>(content, 4)));
    REQUIRE(scene.files.AddFile("Content/Empty.png", Span<const uint8>()));
    const String rock = Under(env.allocator, "Content/Rock.png");
    Array<uint8> bytes(env.allocator);
    Sha256Digest digest;
    AssetDiagnostic failure;
    REQUIRE(ReadImportSource(scene.files, rock.View(), bytes, digest));
    CHECK(bytes.Size() == 4 && digest == Sha256::Hash(Span<const uint8>(content, 4)));
    REQUIRE(ReadImportSource(scene.files, rock.View(), bytes, digest, 4));
    const String empty = Under(env.allocator, "Content/Empty.png");
    REQUIRE(ReadImportSource(scene.files, empty.View(), bytes, digest));
    CHECK(bytes.IsEmpty() && digest == Sha256::Hash(Span<const uint8>()));
    auto tooLarge = ReadImportSource(scene.files, rock.View(), bytes, digest, 3, &failure);
    CHECK(!tooLarge && tooLarge.Error() == ErrorCode::CapacityExceeded &&
          failure.code == AssetDiagnosticCode::InvalidSource);
    const String missing = Under(env.allocator, "Content/Missing.png");
    auto absent = ReadImportSource(scene.files, missing.View(), bytes, digest, MaxImportSourceBytes,
                                   &failure);
    CHECK(!absent && absent.Error() == ErrorCode::NotFound &&
          failure.code == AssetDiagnosticCode::MissingSource);
    scene.files.faults.openRead = MemoryBusy;
    scene.files.faults.readSuffix = "Rock.png";
    auto busy =
        ReadImportSource(scene.files, rock.View(), bytes, digest, MaxImportSourceBytes, &failure);
    scene.files.faults.openRead = {};
    scene.files.faults.readSuffix = nullptr;
    CHECK(!busy && busy.Error() == ErrorCode::Busy &&
          failure.code == AssetDiagnosticCode::InvalidSource);
    // 할당 실패.
    test::NFailAllocator failing(env.allocator);
    Array<uint8> failingBytes(failing);
    failing.FailOnAttempt(1);
    auto oom = ReadImportSource(scene.files, rock.View(), failingBytes, digest,
                                MaxImportSourceBytes, &failure);
    failing.DisableFailure();
    CHECK(!oom && oom.Error() == ErrorCode::OutOfMemory &&
          failure.code == AssetDiagnosticCode::None);
}

// ImportRecord 원본 위치 규칙(4.5절, 7.1절 15·18): 루트가 있으면 상대, 비면 절대(POSIX·UNC·
// 드라이브 — 호스트와 무관), 성분 규칙은 같다.
TEST(ImportRecord_SourceLocations) {
    struct Case {
        StringView root;
        StringView path;
        bool valid;
    };
    const Case cases[] = {
        {"Art", "a/b.png", true},
        {"Art", "/a.png", false},
        {"Art", "C:/a.png", true},
        {"", "/a.png", true},
        {"", "/a/b/c.png", true},
        {"", "//server/share/a.png", true},
        {"", "//server/share", false},
        {"", "//server", false},
        {"", "C:/a.png", true},
        {"", "z:/a.png", true},
        {"", "1:/a.png", false},
        {"", "[:/a.png", false},
        {"", "{:/a.png", false},
        {"", "C:a.png", false},
        {"", "C:/", false},
        {"", "/", false},
        {"", "a.png", false},
        {"", "/a/../b", false},
        {"", "/a//b", false},
        {"", "/a/", false},
        {"", "C:\\a.png", false},
        {"", "", false},
        {"A/B", "a.png", false},
    };
    for (const Case& one : cases) {
        CHECK(IsValidImportSourceLocation(one.root, one.path) == one.valid);
    }
}

// 임포터 표: 등록·찾기(id·이름 대소문자 무시·확장자 대소문자 무시), 같은 id·이름·기본 확장자
// AlreadyExists, 기본 확장자 없는 임포터, 해제(토큰 소멸·Release·이동), 규칙 위반 InvalidArgument,
// 타입 조회(없음 NotFound·종류·추상), 옵션 이름 충돌(속성 — 대소문자만 다름·예약 이름), 할당 실패.
TEST(AssetImporterTable_RegisterAndFind) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AssetImporterTable table(env.allocator, env.types);
    InfoImporter first(ValidInfo());
    AssetImporterInfo secondInfo = ValidInfo();
    secondInfo.id = Guid{0x494d50, 2};
    secondInfo.name = "CubeImporter";
    constexpr StringView ddsExtension[] = {"dds"};
    secondInfo.extensions = ddsExtension;
    secondInfo.roles = CubeRoles;
    secondInfo.optionsType = TypeId{};
    InfoImporter second(secondInfo);
    {
        auto registered = table.Register(first);
        REQUIRE(registered && registered.Value().IsActive());
        auto other = table.Register(second);
        REQUIRE(other);
        InfoImporter sameIdUnregistered(ValidInfo());
        // 기본 확장자가 없는 임포터(이름으로만 고른다).
        AssetImporterInfo plainInfo = ValidInfo();
        plainInfo.id = Guid{0x494d50, 9};
        plainInfo.name = "Plain";
        plainInfo.extensions = {};
        InfoImporter plain(plainInfo);
        auto plainRegistered = table.Register(plain);
        REQUIRE(plainRegistered);
        CHECK(table.FindByName("plain") == &plain && table.FindByExtension("dds") == &second);
        // 등록 동안 쥔 타입 lease(옵션 없는 importer·등록되지 않은 importer는 null).
        CHECK(table.OutputType(first) != nullptr &&
              table.OutputType(first)->Id() == IdOf<TestAsset>() &&
              table.OptionsType(first) != nullptr && table.OptionsType(second) == nullptr &&
              table.OutputType(sameIdUnregistered) == nullptr &&
              table.OptionsType(sameIdUnregistered) == nullptr);
        plainRegistered.Value().Release();
        CHECK(table.Count() == 2 && table.FindById(Guid{0x494d50, 1}) == &first &&
              table.FindByName("test_IMPORTER2") == &first &&
              table.FindByName("CUBEIMPORTER") == &second &&
              table.FindByExtension("PNG") == &first && table.FindByExtension("tga") == &first &&
              table.FindByExtension("jpg") == nullptr && table.FindByName("Nope") == nullptr &&
              table.FindById(Guid{1, 1}) == nullptr);
        // 같은 id·이름·기본 확장자.
        AssetImporterInfo clash = ValidInfo();
        clash.name = "Other";
        clash.extensions = {};
        InfoImporter sameId(clash);
        CHECK(table.Register(sameId).Error() == ErrorCode::AlreadyExists);
        clash.id = Guid{0x494d50, 3};
        clash.name = "TEST_importer2";
        InfoImporter sameName(clash);
        CHECK(table.Register(sameName).Error() == ErrorCode::AlreadyExists);
        constexpr StringView tgaOnly[] = {"tga"};
        clash.name = "Other";
        clash.extensions = tgaOnly;
        InfoImporter sameExtension(clash);
        CHECK(table.Register(sameExtension).Error() == ErrorCode::AlreadyExists);
        // 이동·Release.
        AssetImporterRegistration moved = rocks::Move(other.Value());
        CHECK(!other.Value().IsActive() && moved.IsActive());
        moved.Release();
        CHECK(table.Count() == 1 && table.FindByName("CubeImporter") == nullptr);
        AssetImporterRegistration assigned;
        assigned = rocks::Move(registered.Value());
        CHECK(assigned.IsActive() && table.Count() == 1);
        assigned = AssetImporterRegistration{};
        CHECK(table.Count() == 0);
    }

    // 토큰 소멸이 해제한다.
    {
        auto registered = table.Register(first);
        REQUIRE(registered);
        CHECK(table.Count() == 1);
    }

    CHECK(table.Count() == 0);

    // 규칙 위반.
    auto rejects = [&](AssetImporterInfo info, ErrorCode error) noexcept {
        InfoImporter importer(info);
        auto registered = table.Register(importer);
        return !registered && registered.Error() == error && table.Count() == 0;
    };
    AssetImporterInfo info = ValidInfo();
    info.id = Guid{};
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    const StringView badNames[] = {"", "Bad-Name", "a b", "a{", "a:", "a[", "a`", "a.b"};
    for (StringView name : badNames) {
        info = ValidInfo();
        info.name = name;
        CHECK(rejects(info, ErrorCode::InvalidArgument));
    }

    static char longName[MaxImporterNameBytes + 2] = {};
    for (usize index = 0; index <= MaxImporterNameBytes; ++index) {
        longName[index] = 'n';
    }

    info = ValidInfo();
    info.name = StringView(longName, MaxImporterNameBytes + 1);
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    info.name = StringView(longName, MaxImporterNameBytes);
    {
        InfoImporter longest(info);
        auto registered = table.Register(longest);
        CHECK(registered);
    }

    const StringView badExtensions[][2] = {
        {"PNG", "x"},   {"", "x"},    {"p.g", "x"},
        {"png", "png"}, {"a_b", "x"}, {"a:b", "x"},
        {"a{b", "x"},   {"a/b", "x"}, {"x", "abcdefghijklmnopq"}};
    for (const auto& pair : badExtensions) {
        info = ValidInfo();
        info.extensions = Span<const StringView>(pair, 2);
        CHECK(rejects(info, ErrorCode::InvalidArgument));
    }

    static StringView manyExtensions[MaxImporterExtensions + 1];
    static char extensionText[MaxImporterExtensions + 1][4];
    for (usize index = 0; index <= MaxImporterExtensions; ++index) {
        extensionText[index][0] = 'e';
        extensionText[index][1] = static_cast<char>('a' + index / 26);
        extensionText[index][2] = static_cast<char>('a' + index % 26);
        manyExtensions[index] = StringView(extensionText[index], 3);
    }

    info = ValidInfo();
    info.extensions = Span<const StringView>(manyExtensions, MaxImporterExtensions + 1);
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    info.extensions = Span<const StringView>(manyExtensions, MaxImporterExtensions);
    {
        InfoImporter widest(info);
        auto registered = table.Register(widest);
        CHECK(registered);
    }

    const StringView badRoles[][2] = {{"px", "px"}, {"", "x"}, {"a/b", "x"}, {"a-b", "x"}};
    for (const auto& pair : badRoles) {
        info = ValidInfo();
        info.roles = Span<const StringView>(pair, 2);
        CHECK(rejects(info, ErrorCode::InvalidArgument));
    }

    static StringView manyRoles[MaxImportSources + 1];
    static char roleText[MaxImportSources + 1][8];
    for (usize index = 0; index <= MaxImportSources; ++index) {
        FixedStringBuilder role{Span<char>(roleText[index])};
        CHECK(role.TryAppendFormat("r{}", index));
        manyRoles[index] = role.View();
    }

    info = ValidInfo();
    info.roles = Span<const StringView>(manyRoles, MaxImportSources + 1);
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    // 타입: 주 출력 없음·struct·에셋 아닌 클래스·추상 에셋, 옵션 없음·클래스·충돌·예약 이름.
    info = ValidInfo();
    info.outputType = Guid{0x7777, 1};
    CHECK(rejects(info, ErrorCode::NotFound));
    info.outputType = GeneratedType<TestImportOptions>::Id();
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    info.outputType = IdOf<TestPart>();
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    info.outputType = GeneratedType<Asset>::Id();
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    info = ValidInfo();
    info.optionsType = Guid{0x7777, 2};
    CHECK(rejects(info, ErrorCode::NotFound));
    info.optionsType = IdOf<TestAsset>();
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    info.optionsType = GeneratedType<CollidingImportOptions>::Id();
    CHECK(rejects(info, ErrorCode::InvalidArgument));
    info.optionsType = GeneratedType<ReservedImportOptions>::Id();
    CHECK(rejects(info, ErrorCode::InvalidArgument));

    // 할당 실패.
    test::NFailAllocator failing(env.allocator);
    AssetImporterTable failingTable(failing, env.types);
    failing.FailOnAttempt(1);
    auto oom = failingTable.Register(first);
    failing.DisableFailure();
    CHECK(!oom && oom.Error() == ErrorCode::OutOfMemory && failingTable.Count() == 0);
}

} // namespace asset_import_foundation_tests_file
