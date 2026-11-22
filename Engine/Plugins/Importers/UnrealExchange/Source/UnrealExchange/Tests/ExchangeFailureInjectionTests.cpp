#include "ExchangeTestSupport.h"

#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Unreal 교환 리더·순서의 실패 주입 시험이다(Docs/Design/UnrealExchange.ko.md 12.1절, 10.4절 끝
// coverage 문단): 진단 목록 할당 실패를 진단을 내는 모든 자리에서 일으키고, 긴 묶음 경로의 경로
// 조립 할당 실패, 조각 경계 뒤의 읽기 실패, blob 사용 중복·충돌의 조합, 순서 계산의 경계 그래프를
// 본다.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_failure_injection_tests_file {

constexpr usize LongDirectoryBytes = 600;
constexpr uint32 LargeLayoutCount = 20000;

constexpr StringView PixelsHex = "0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8";
constexpr StringView SlotNamesHex =
    "0f88646c73f1452bd75860b1581d665ce9d289117aa4da99bc4e27a237467769";

// fixture 편집 하나다(from이 비어 있으면 file을 지운다).
struct FixtureEdit {
    StringView file;
    StringView from;
    StringView to;
};

bool ApplyEdits(Allocator& allocator, MemoryFiles& files, Span<const FixtureEdit> edits) noexcept {
    for (const FixtureEdit& edit : edits) {
        StringBuilder<256> path(allocator);
        if (!FixtureFile(edit.file, path)) {
            return false;
        }

        const bool applied = edit.from.IsEmpty()
                                 ? files.Remove(path.View())
                                 : files.ReplaceOnce(path.View(), edit.from, edit.to);
        if (!applied) {
            return false;
        }
    }

    return true;
}

// 진단 목록의 n번째 할당을 실패시키며 읽기를 되풀이한다. 실패는 OutOfMemory이고 묶음은 비어 있다.
// 끝까지 가면 진단 수가 기대값과 같다. 실패 횟수를 돌려준다.
uint32 ExhaustReadDiagnostics(Allocator& allocator, MemoryFiles& files,
                              usize expectedDiagnostics) noexcept {
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(allocator);
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(failing);
        failing.FailOnAttempt(attempt);
        auto read = ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics);
        if (!read && read.Error() == ErrorCode::OutOfMemory) {
            CHECK(bundle.Assets().IsEmpty() && files.OpenHandles() == 0);
            ++failures;
            continue;
        }

        if (failing.Attempts() < attempt) {
            CHECK(diagnostics.Items().Size() == expectedDiagnostics);
            return failures;
        }
    }
}

// 진단을 내는 자리마다(모르는 키 9곳, 범위 밖 클래스, blob 충돌, 의존 class 불일치, blob 파일 없음,
// 버전 오류) 진단 목록 할당 실패가 OutOfMemory로 끝난다.
TEST(UnrealExchange_ReadDiagnosticExhaustion) {
    SystemAllocator allocator;
    {
        const FixtureEdit unknown[] = {
            {"bundle.json", "\"createdUtc\":", "\"comment\": \"hello\", \"createdUtc\":"},
            {"bundle.json", "\"plugin\": \"RocksExport\",",
             "\"plugin\": \"RocksExport\", \"b\": 7,"},
            {"bundle.json", "\"substrate\": false,", "\"substrate\": false, \"n\": true,"},
            {"bundle.json", "\"from\": \"/Game/Old/MI_Old.MI_Old\",",
             "\"from\": \"/Game/Old/MI_Old.MI_Old\", \"r\": 1,"},
            {"bundle.json", "\"file\": \"assets/Game/Textures/T_Rock.json\"",
             "\"file\": \"assets/Game/Textures/T_Rock.json\", \"s\": 1"},
            {"assets/Game/Textures/T_Rock.json", "\"count\": 4", "\"count\": 4, \"t\": 4"},
            {"assets/Game/Textures/T_Rock.json", "\"settings\": {", "\"e\": [], \"settings\": {"},
            {"assets/Game/Materials/MI_Rock.json", "\"kind\": \"hard\"",
             "\"kind\": \"hard\", \"v\": 0"},
            {"assets/Game/Characters/SK_Hero.json", "\"severity\": \"info\",",
             "\"severity\": \"info\", \"l\": 3,"},
        };
        const FixtureEdit conflict[] = {
            {"assets/Game/Textures/T_Rock.json", PixelsHex,
             "4fb8f2852abd9d09c1bffa387cb002163d2be61141df31898e80fe15aa"
             "f76148"}};
        const FixtureEdit mismatch[] = {{"assets/Game/Materials/MI_Rock.json",
                                         "\"class\": \"Material\",", "\"class\": \"Texture2D\","}};
        const FixtureEdit missingBlob[] = {
            {"blobs/03/0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8.bin", "",
             ""}};
        const FixtureEdit version[] = {
            {"bundle.json", "\"formatVersion\": 1,", "\"formatVersion\": 2,"}};
        struct Scenario {
            Span<const FixtureEdit> edits;
            usize diagnostics;
        };
        const Scenario scenarios[] = {
            {Span<const FixtureEdit>(), 1},
            {Span<const FixtureEdit>(unknown), 10},
            {Span<const FixtureEdit>(conflict), 5},
            {Span<const FixtureEdit>(mismatch), 2},
            {Span<const FixtureEdit>(missingBlob), 3},
            {Span<const FixtureEdit>(version), 1},
        };
        for (const Scenario& scenario : scenarios) {
            MemoryFiles files(allocator);
            REQUIRE(LoadFixture(allocator, files));
            REQUIRE(ApplyEdits(allocator, files, scenario.edits));
            CHECK(ExhaustReadDiagnostics(allocator, files, scenario.diagnostics) >=
                  scenario.diagnostics);
        }

        // LoadExchangeBlob의 진단 할당 실패.
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        StringBuilder<256> blob(allocator);
        REQUIRE(FixtureFile(
            "blobs/03/0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8.bin", blob));
        REQUIRE(files.Remove(blob.View()));
        test::NFailAllocator failing(allocator);
        ExchangeDiagnostics noisy(failing);
        Array<uint8> out(allocator);
        failing.FailOnAttempt(1);
        CHECK(LoadExchangeBlob(bundle, 9, bundle.Assets()[9].blobs[0], out, noisy).Error() ==
              ErrorCode::OutOfMemory);
        failing.DisableFailure();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 순서 계산이 진단을 내는 자리마다 진단 목록 할당 실패를 일으킨다. 실패는 OutOfMemory이고 결과는
// 비어 있다.
uint32 ExhaustPlanDiagnostics(Allocator& allocator, const ExchangeBundle& bundle,
                              const ExchangeOrderOptions& options) noexcept {
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(allocator);
        ExchangeDiagnostics diagnostics(failing);
        ExchangeImportPlan plan(allocator);
        failing.FailOnAttempt(attempt);
        auto planned = PlanExchangeImport(bundle, options, plan, diagnostics);
        if (!planned && planned.Error() == ErrorCode::OutOfMemory) {
            CHECK(plan.Units().IsEmpty());
            ++failures;
            continue;
        }

        if (failing.Attempts() < attempt) {
            return failures;
        }
    }
}

bool ReadSynthetic(Allocator& allocator, MemoryFiles& files, Span<const SyntheticAsset> assets,
                   ExchangeBundle& bundle) noexcept {
    ExchangeDiagnostics diagnostics(allocator);
    return WriteSyntheticBundle(allocator, files, assets) &&
           ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics);
}

TEST(UnrealExchange_PlanDiagnosticExhaustion) {
    SystemAllocator allocator;
    {
        // fixture(이름 바꿈 정보, 누락 텍스처 경고), 잘못된 root.
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        ExchangeBundle fixture(allocator);
        ExchangeDiagnostics quiet(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, fixture, quiet));
        CHECK(ExhaustPlanDiagnostics(allocator, fixture, {}) >= 2);
        ExchangeOrderOptions bad;
        bad.root = "/Game/Bad Root";
        CHECK(ExhaustPlanDiagnostics(allocator, fixture, bad) >= 1);

        // Build 순환·전파·누락 의존·경로 겹침·경로 초과(단위 구성원 실패).
        StringBuilder<512> longPath(allocator);
        REQUIRE(longPath.TryAppend("/Game/P"));
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(longPath.TryAppendAscii('/') &&
                    longPath.TryAppendRepeated(static_cast<char>('a' + index), 60));
        }

        REQUIRE(longPath.TryAppend("/A.A"));
        const SyntheticDependency toB[] = {{"/Game/M/MI_B.MI_B", "MaterialInstanceConstant"}};
        const SyntheticDependency toA[] = {{"/Game/M/MI_A.MI_A", "MaterialInstanceConstant"}};
        const SyntheticDependency missing[] = {{"/Game/X/S.S", "Skeleton"},
                                               {"/Game/X/T.T", "Texture2D"}};
        const SyntheticDependency toShort[] = {{"/Game/Q/B.B", "StaticMesh"}};
        const SyntheticDependency toLong[] = {{longPath.View(), "StaticMesh"}};
        const SyntheticDependency toY[] = {{"/Game/X/S.S", "Skeleton"},
                                           {"/Game/Y/Y.Y", "StaticMesh"}};
        const SyntheticDependency toX[] = {{"/Game/Y/X.X", "StaticMesh"}};
        const SyntheticAsset assets[] = {
            {"/Game/K/Rock.Rock", "StaticMesh", {}},
            {"/Game/K/rock.rock", "StaticMesh", {}},
            {"/Game/M/MI_A.MI_A", "MaterialInstanceConstant", Span<const SyntheticDependency>(toB)},
            {"/Game/M/MI_B.MI_B", "MaterialInstanceConstant", Span<const SyntheticDependency>(toA)},
            {longPath.View(), "StaticMesh", Span<const SyntheticDependency>(toShort)},
            {"/Game/Q/B.B", "StaticMesh", Span<const SyntheticDependency>(toLong)},
            {"/Game/S/Mesh.Mesh", "SkeletalMesh", Span<const SyntheticDependency>(missing)},
            {"/Game/S/SM.SM", "StaticMesh", Span<const SyntheticDependency>(toA)},
            {"/Game/Y/X.X", "StaticMesh", Span<const SyntheticDependency>(toY)},
            {"/Game/Y/Y.Y", "StaticMesh", Span<const SyntheticDependency>(toX)},
        };
        MemoryFiles synthetic(allocator);
        ExchangeBundle bundle(allocator);
        REQUIRE(ReadSynthetic(allocator, synthetic, Span<const SyntheticAsset>(assets), bundle));
        CHECK(ExhaustPlanDiagnostics(allocator, bundle, {}) >= 10);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 긴 묶음 디렉터리(경로 builder가 할당한다)에서 읽기·blob 읽기의 할당 실패, 빈 디렉터리와 `\`로
// 끝나는 디렉터리.
TEST(UnrealExchange_DirectoryPathForms) {
    SystemAllocator allocator;
    {
        StringBuilder<16> directory(allocator);
        REQUIRE(directory.TryAppendRepeated('d', LongDirectoryBytes));
        StringBuilder<512> source(allocator);
        REQUIRE(FixtureSourcePath("Basic.rxbundle", source));
        MemoryFiles files(allocator);
        REQUIRE(CopyTree(allocator, source.View(), directory.View(), files));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            ExchangeBundle bundle(failing);
            ExchangeDiagnostics diagnostics(allocator);
            failing.FailOnAttempt(attempt);
            auto read = ReadExchangeBundle(files, directory.View(), {}, bundle, diagnostics);
            if (!read) {
                CHECK(read.Error() == ErrorCode::OutOfMemory && bundle.Assets().IsEmpty());
                ++failures;
                continue;
            }

            CHECK(bundle.Assets().Size() == 10 && diagnostics.Items().Size() == 1);
            if (failing.Attempts() < attempt) {
                // 읽은 뒤 blob 경로 조립의 할당 실패.
                Array<uint8> out(allocator);
                failing.FailOnAttempt(1);
                CHECK(LoadExchangeBlob(bundle, 9, bundle.Assets()[9].blobs[0], out, diagnostics)
                          .Error() == ErrorCode::OutOfMemory);
                failing.DisableFailure();
                REQUIRE(LoadExchangeBlob(bundle, 9, bundle.Assets()[9].blobs[0], out, diagnostics));
                CHECK(out.Size() == 16);
                break;
            }
        }

        CHECK(failures > 50);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(files.PutText("bundle.json", "[]"));
        REQUIRE(files.PutText("X\\bundle.json", "[1]"));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadExchangeBundle(files, "", {}, bundle, diagnostics).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ReadExchangeBundle(files, "X\\", {}, bundle, diagnostics).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::WrongType) == 2);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 큰 가변 배치 blob을 쓰고 그 파일에 fault를 건 합성 묶음을 읽어 첫 진단 세부 규칙을 돌려준다.
ExchangeIssue ReadFaultedBlob(Allocator& allocator, const BlobBytes& bytes, StringView layout,
                              uint64 count, FileFault fault) noexcept {
    MemoryFiles files(allocator);
    StringBuilder<256> data(allocator);
    if (!data.TryAppend("{\"values\": ") ||
        !WriteSyntheticBlob(files, bytes.bytes.View(), layout, 1, count, data) ||
        !data.TryAppendAscii('}')) {
        return ExchangeIssue::None;
    }

    const SyntheticAsset assets[] = {{"/Game/S/A.A", "SkeletalMesh", {}, data.View()}};
    if (!WriteSyntheticBundle(allocator, files, Span<const SyntheticAsset>(assets))) {
        return ExchangeIssue::None;
    }

    const Sha256Digest digest = Sha256::Hash(bytes.bytes.View());
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    StringBuilder<256> path(allocator);
    if (!path.TryAppend(SyntheticDirectory) || !path.TryAppend("/blobs/") ||
        !path.TryAppend(StringView(hex, 2)) || !path.TryAppendAscii('/') ||
        !path.TryAppend(StringView(hex, Sha256Digest::HexBytes)) || !path.TryAppend(".bin") ||
        !files.faultPath.TryAssign(path.View())) {
        return ExchangeIssue::None;
    }

    files.fault = fault;
    ExchangeBundle bundle(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    if (!ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics)) {
        return ExchangeIssue::None;
    }

    const ExchangeDiagnostic* error = diagnostics.Find(AssetDiagnosticCode::CorruptBundle);
    return error != nullptr ? error->issue : ExchangeIssue::None;
}

// 읽기 실패가 배치 파싱 도중(첫 word, 첫 조각 뒤)에 나도 ReadFailed다. 조각 경계를 넘는 큰 배치는
// 실패가 없으면 통과한다.
TEST(UnrealExchange_BlobReadFaults) {
    SystemAllocator allocator;
    {
        BlobBytes influences(allocator);
        for (uint32 vertex = 0; vertex <= LargeLayoutCount; ++vertex) {
            REQUIRE(influences.Word(vertex));
        }

        REQUIRE(influences.Fill(static_cast<usize>(LargeLayoutCount) * 4, 3));
        BlobBytes sparse(allocator);
        REQUIRE(sparse.Word(LargeLayoutCount));
        for (uint32 index = 0; index < LargeLayoutCount; ++index) {
            REQUIRE(sparse.Word(index));
        }

        REQUIRE(sparse.Fill(static_cast<usize>(LargeLayoutCount) * 12, 0x40));
        BlobBytes names(allocator);
        for (uint32 index = 0; index < LargeLayoutCount; ++index) {
            REQUIRE(names.Text("name"));
        }

        const FileFault faults[] = {FileFault::None, FileFault::Read, FileFault::ReadLate};
        for (const FileFault fault : faults) {
            const ExchangeIssue expected =
                fault == FileFault::None ? ExchangeIssue::None : ExchangeIssue::ReadFailed;
            CHECK(ReadFaultedBlob(allocator, influences, "boneInfluences", LargeLayoutCount,
                                  fault) == expected);
            CHECK(ReadFaultedBlob(allocator, sparse, "sparseVec3", LargeLayoutCount, fault) ==
                  expected);
            CHECK(ReadFaultedBlob(allocator, names, "names", LargeLayoutCount, fault) == expected);
        }

        // 가변 배치 중간에 데이터가 끝난다(두 번째 길이 word가 2 byte뿐).
        BlobBytes cut(allocator);
        REQUIRE(cut.Text("ab") && cut.Fill(2, 0));
        CHECK(ReadFaultedBlob(allocator, cut, "names", 2, FileFault::None) ==
              ExchangeIssue::BlobLayout);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// blob 사용 조합: 같은 blob을 다른 components로·같은 사용을 두 번 적어도 Ready이고 서로 다른 사용만
// 남는다. 같은 배치·components에 count만 다른 사용은 따로 검증한다. 충돌로 이미 실패한 에셋은 뒤의
// blob 파일 실패로 다시 진단하지 않는다.
TEST(UnrealExchange_BlobUseCombinations) {
    SystemAllocator allocator;
    {
        const FixtureEdit reuse[] = {
            {"assets/Game/Textures/T_Rock.json", "\"source\": {",
             "\"source\": {\"extra\": [{\"blob\": "
             "\"0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8\", \"bytes\": 16, "
             "\"layout\": \"u8\", \"components\": 4, \"count\": 4}, {\"blob\": "
             "\"0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8\", \"bytes\": 16, "
             "\"layout\": \"u8\", \"components\": 2, \"count\": 8}], "}};
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(ApplyEdits(allocator, files, Span<const FixtureEdit>(reuse)));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        CHECK(bundle.Assets()[9].state == ExchangeAssetState::Ready);
        REQUIRE(bundle.Assets()[9].blobs.Size() == 2);
        CHECK(bundle.Assets()[9].blobs[1].components == 2);
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 0);
    }

    {
        const FixtureEdit recount[] = {
            {"assets/Game/Meshes/SM_Rock.json", "\"data\": {",
             "\"data\": {\"again\": {\"blob\": "
             "\"0f88646c73f1452bd75860b1581d665ce9d289117aa4da99bc4e27a237467769\", \"bytes\": 16, "
             "\"layout\": \"names\", \"components\": 1, \"count\": 1}, "}};
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(ApplyEdits(allocator, files, Span<const FixtureEdit>(recount)));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        CHECK(bundle.Assets()[6].state == ExchangeAssetState::Failed);
        const ExchangeDiagnostic* error = FindAssetDiagnostic(diagnostics, 6);
        REQUIRE(error != nullptr);
        CHECK(error->issue == ExchangeIssue::BlobLayout && error->text.View() == SlotNamesHex);
    }

    {
        const FixtureEdit doubled[] = {
            {"assets/Game/Textures/T_Rock.json", "\"source\": {",
             "\"source\": {\"extra\": {\"blob\": "
             "\"4fb8f2852abd9d09c1bffa387cb002163d2be61141df31898e80fe15aaf76148\", \"bytes\": 16, "
             "\"layout\": \"u8\", \"components\": 4, \"count\": 4}, "},
            {"blobs/03/0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8.bin", "",
             ""}};
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(ApplyEdits(allocator, files, Span<const FixtureEdit>(doubled)));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        CHECK(bundle.Assets()[9].state == ExchangeAssetState::Failed);
        CHECK(bundle.Assets()[8].state == ExchangeAssetState::Failed);
        usize textureErrors = 0;
        for (const ExchangeDiagnostic& item : diagnostics.Items()) {
            textureErrors += item.asset == 9 ? 1 : 0;
        }

        CHECK(textureErrors == 1);
        CHECK(FindAssetDiagnostic(diagnostics, 9)->issue == ExchangeIssue::BlobConflict);
        CHECK(FindAssetDiagnostic(diagnostics, 8)->issue == ExchangeIssue::ReadFailed);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 순서 계산 경계: 묶음에 없는 Material 대상은 경고, 읽기 실패 에셋의 경로 초과·겹침은 읽기 실패가
// 먼저, 실패한 의존이 둘인 구성원은 첫 의존을 원인으로, Tarjan의 늦은 역간선(low가 이미 더 작음).
TEST(UnrealExchange_PlanEdgeCases) {
    SystemAllocator allocator;
    {
        StringBuilder<512> longPath(allocator);
        REQUIRE(longPath.TryAppend("/Game/P"));
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(longPath.TryAppendAscii('/') &&
                    longPath.TryAppendRepeated(static_cast<char>('a' + index), 60));
        }

        REQUIRE(longPath.TryAppend("/A.A"));
        const SyntheticDependency toMaterial[] = {{"/Game/X/M.M", "Material"}};
        const SyntheticDependency meshB[] = {{"/Game/C/B.B", "StaticMesh"}};
        const SyntheticDependency meshC[] = {{"/Game/C/C.C", "StaticMesh"}};
        const SyntheticDependency meshAB[] = {{"/Game/C/A.A", "StaticMesh"},
                                              {"/Game/C/B.B", "StaticMesh"}};
        const SyntheticAsset assets[] = {
            {"/Game/C/A.A", "StaticMesh", Span<const SyntheticDependency>(meshB)},
            {"/Game/C/B.B", "StaticMesh", Span<const SyntheticDependency>(meshC)},
            {"/Game/C/C.C", "StaticMesh", Span<const SyntheticDependency>(meshAB)},
            {"/Game/K/Rock.Rock", "StaticMesh", {}},
            {"/Game/K/rock.rock", "StaticMesh", {}},
            {longPath.View(), "StaticMesh", {}},
            {"/Game/S/SM.SM", "StaticMesh", Span<const SyntheticDependency>(toMaterial)},
        };
        MemoryFiles files(allocator);
        REQUIRE(WriteSyntheticBundle(allocator, files, Span<const SyntheticAsset>(assets)));
        // 겹치는 둘 중 하나와 긴 경로 에셋의 JSON을 망가뜨린다.
        StringBuilder<512> path(allocator);
        REQUIRE(path.TryAppend(SyntheticDirectory) && path.TryAppend("/assets/Game/K/Rock.json"));
        REQUIRE(files.PutText(path.View(), "{"));
        path.Clear();
        REQUIRE(path.TryAppend(SyntheticDirectory) && path.TryAppend("/assets") &&
                path.TryAppend(UnrealPackagePath(longPath.View())) && path.TryAppend(".json"));
        REQUIRE(files.PutText(path.View(), "{"));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        const ExchangeImportUnit& cycle = plan.Units()[plan.UnitOf(0)];
        CHECK(cycle.memberCount == 3 && cycle.state == ExchangeUnitState::Failed &&
              cycle.failure == AssetDiagnosticCode::UnsupportedCycle);
        CHECK(plan.Units()[plan.UnitOf(3)].failure == AssetDiagnosticCode::CorruptBundle);
        CHECK(plan.Units()[plan.UnitOf(4)].failure == AssetDiagnosticCode::UnrealPathCollision);
        CHECK(plan.Units()[plan.UnitOf(5)].failure == AssetDiagnosticCode::CorruptBundle);
        CHECK(plan.Units()[plan.UnitOf(6)].state == ExchangeUnitState::Ready);
        CHECK(diagnostics.Count(AssetDiagnosticCode::MissingUnrealDependency) == 1);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Materials/M_Base.json", path));
        REQUIRE(files.PutText(path.View(), "{"));
        path.Clear();
        REQUIRE(FixtureFile("assets/Game/Characters/SKEL_Hero.json", path));
        REQUIRE(files.PutText(path.View(), "{"));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        const ExchangeImportUnit& skinned = plan.Units()[plan.UnitOf(1)];
        CHECK(skinned.failure == AssetDiagnosticCode::MissingDependency &&
              skinned.failureAsset == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 순서 계산의 할당 실패: 결합 문자가 든 root(검사에 할당이 든다)와 깊은 사슬(Tarjan stack·frame이
// 자란다).
TEST(UnrealExchange_PlanAllocationEdges) {
    SystemAllocator allocator;
    {
        Array<String> names(allocator);
        for (uint32 index = 0; index < 40; ++index) {
            StringBuilder<64> name(allocator);
            String copy(allocator);
            REQUIRE(name.TryAppendFormat("/Game/C/A{:03}.A", index) &&
                    copy.TryAssign(name.View()) && names.TryPushBack(rocks::Move(copy)));
        }

        Array<SyntheticDependency> links(allocator);
        Array<SyntheticAsset> assets(allocator);
        REQUIRE(links.TryResize(names.Size()));
        for (usize index = 0; index < names.Size(); ++index) {
            links[index] =
                SyntheticDependency{names[(index + 1) % names.Size()].View(), "StaticMesh"};
            REQUIRE(assets.TryPushBack(
                SyntheticAsset{names[index].View(), "StaticMesh",
                               Span<const SyntheticDependency>(&links[index], 1)}));
        }

        MemoryFiles files(allocator);
        ExchangeBundle bundle(allocator);
        REQUIRE(ReadSynthetic(allocator, files, assets.View(), bundle));
        ExchangeOrderOptions options;
        options.root = "/Game/x\xCC\x81";
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            ExchangeImportPlan plan(failing);
            ExchangeDiagnostics diagnostics(allocator);
            failing.FailOnAttempt(attempt);
            auto planned = PlanExchangeImport(bundle, options, plan, diagnostics);
            if (!planned) {
                CHECK(planned.Error() == ErrorCode::OutOfMemory && plan.Units().IsEmpty());
                CHECK(diagnostics.Find(AssetDiagnosticCode::InvalidPath) == nullptr);
                ++failures;
                continue;
            }

            CHECK(plan.Units().Size() == 1 && plan.Units()[0].memberCount == names.Size());
            if (failing.Attempts() < attempt) {
                break;
            }
        }

        CHECK(failures > 20);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 목록 상한(10.4절 16번): 에셋·redirect가 UnrealExchangeMaxAssets·MaxRedirects를 넘으면 항목을 보기
// 전에 묶음 수준 CorruptBundle(LimitExceeded)이다.
TEST(UnrealExchange_ManifestListLimits) {
    SystemAllocator allocator;
    {
        const StringView keys[] = {"\"assets\": [", "\"redirects\": ["};
        const uint32 limits[] = {UnrealExchangeMaxAssets, UnrealExchangeMaxRedirects};
        for (usize which = 0; which < 2; ++which) {
            StringBuilder<256> list(allocator);
            REQUIRE(list.TryAppend(keys[which]));
            for (uint32 index = 0; index <= limits[which]; ++index) {
                REQUIRE(list.TryAppend("0,"));
            }

            MemoryFiles files(allocator);
            REQUIRE(LoadFixture(allocator, files));
            StringBuilder<256> path(allocator);
            REQUIRE(FixtureFile("bundle.json", path));
            REQUIRE(files.ReplaceOnce(path.View(), keys[which], list.View()));
            ExchangeBundle bundle(allocator);
            ExchangeDiagnostics diagnostics(allocator);
            CHECK(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                              ExchangeIssue::LimitExceeded) == 1);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 넓은 settings·data(blob 참조를 찾는 stack이 자란다)에서 묶음 allocator 할당 실패.
TEST(UnrealExchange_WideDataAllocationFailures) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        BlobBytes blob(allocator);
        REQUIRE(blob.Fill(8, 1));
        StringBuilder<256> data(allocator);
        REQUIRE(data.TryAppend("{\"pixels\": "));
        REQUIRE(WriteSyntheticBlob(files, blob.bytes.View(), "u8", 2, 4, data));
        for (uint32 index = 0; index < 64; ++index) {
            REQUIRE(data.TryAppendFormat(", \"k{}\": [{}, {{\"v\": {}}}]", index, index, index));
        }

        REQUIRE(data.TryAppendAscii('}'));
        const SyntheticAsset assets[] = {{"/Game/T/Wide.Wide", "Texture2D", {}, data.View()}};
        REQUIRE(WriteSyntheticBundle(allocator, files, Span<const SyntheticAsset>(assets)));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            ExchangeBundle bundle(failing);
            ExchangeDiagnostics diagnostics(allocator);
            failing.FailOnAttempt(attempt);
            auto read = ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics);
            if (!read) {
                CHECK(read.Error() == ErrorCode::OutOfMemory && bundle.Assets().IsEmpty());
                ++failures;
                continue;
            }

            CHECK(bundle.Assets()[0].state == ExchangeAssetState::Ready &&
                  bundle.Assets()[0].blobs.Size() == 1);
            if (failing.Attempts() < attempt) {
                break;
            }
        }

        CHECK(failures > 10);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_failure_injection_tests_file
