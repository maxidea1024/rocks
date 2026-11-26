#include "../../../../../../Source/Developer/AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../../../../../Source/Runtime/AssetRuntime/Tests/AssetTestSupport.h"

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/PackageTags.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeIndexLookup.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// AssetIndex 태그 기반 기존 에셋 조회 시험이다(Docs/Design/UnrealExchange 9.2절, 10.5절
// 4번): 지금 경로 태그, 옛 경로로 남은 경로 태그, 옛 경로 목록 태그(지금·옛 경로), 첫 단계 우선,
// 같은 에셋이 여러 키로 맞음, 모호함(두 에셋·같은 id 두 파일·둘째 단계), 없음, 할당 실패.
using namespace rocks;
using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
namespace exchange_index_lookup_tests_file {

namespace {

// id 패키지(대표 TestAsset)에 tags를 단 에디터 패키지 byte다(실패하면 빈 배열).
Array<uint8> TaggedPackage(AssetEnv& env, uint64 id, Span<const PackageTagInput> tags) noexcept {
    ObjectRegistry& registry = *env.source;
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(id), PackageKind::Editor);
    if (!made) {
        return Array<uint8>(env.allocator);
    }

    RootRef<Package> package = Move(made.Value());
    if (Make<TestAsset>(registry, TestAssetType(), package.Handle(), "").IsNull()) {
        return Array<uint8>(env.allocator);
    }

    auto written = WritePackageTags(env.allocator, tags);
    if (!written) {
        return Array<uint8>(env.allocator);
    }

    PackageWriteOptions options = EditorOptions();
    options.tags = written.Value().View();
    return Write(env.allocator, registry, package.Handle(), options);
}

bool AddPackage(AssetEnv& env, const TempProject& project, StringView file, uint64 id,
                Span<const PackageTagInput> tags) noexcept {
    const Array<uint8> bytes = TaggedPackage(env, id, tags);
    return !bytes.IsEmpty() && WriteContentFile(env.allocator, project, file, bytes.View());
}

bool IsFound(const Result<ExchangeExistingMatch>& match, uint64 id, StringView path,
             bool former = false) noexcept {
    return match && match.Value().state == ExchangeExistingState::Found &&
           match.Value().id == FixedId(id) && match.Value().path == path &&
           match.Value().former == former;
}

bool IsState(const Result<ExchangeExistingMatch>& match, ExchangeExistingState state) noexcept {
    return match && match.Value().state == state && match.Value().id.IsNull() &&
           match.Value().path.IsEmpty();
}

} // namespace

TEST(UnrealExchange_IndexLookup) {
    AssetEnv env;
    REQUIRE(env.Ready());
    TempProject project("exchange-lookup");
    REQUIRE(project.made);
    const PackageTagInput texture[] = {{UnrealExchangeObjectPathTag, "/Game/Tex/T.T"}};
    const PackageTagInput renamed[] = {{UnrealExchangeObjectPathTag, "/Game/Old/Renamed.Renamed"}};
    const PackageTagInput moved[] = {{UnrealExchangeFormerPathsTag, "/Game/Older/X.X"},
                                     {UnrealExchangeObjectPathTag, "/Game/Somewhere/X.X"}};
    const PackageTagInput duplicate[] = {{UnrealExchangeObjectPathTag, "/Game/Dup/D.D"}};
    const PackageTagInput current[] = {{UnrealExchangeObjectPathTag, "/Game/Mixed/M.M"}};
    const PackageTagInput former[] = {{UnrealExchangeFormerPathsTag, "/Game/Mixed/M.M"}};
    const PackageTagInput twoFormer[] = {{UnrealExchangeFormerPathsTag, "/Game/G/Old1.Old1"},
                                         {UnrealExchangeFormerPathsTag, "/Game/G/Old2.Old2"}};
    const PackageTagInput same[] = {{UnrealExchangeObjectPathTag, "/Game/Same/S.S"}};
    const PackageTagInput oneSided[] = {{UnrealExchangeObjectPathTag, "/Game/One/O.O"}};
    // 교환 태그가 아닌 태그는 정렬 표에 들지 않는다.
    const PackageTagInput otherTags[] = {{"Kind", "/Game/Tex/T.T"}};
    const PackageTagInput vTags[] = {{UnrealExchangeObjectPathTag, "/Game/V1.V1"},
                                     {UnrealExchangeFormerPathsTag, "/Game/V2.V2"}};
    const PackageTagInput wTags[] = {{UnrealExchangeObjectPathTag, "/Game/V3.V3"}};
    REQUIRE(AddPackage(env, project, "A.rasset", 1, Span<const PackageTagInput>(texture)));
    REQUIRE(AddPackage(env, project, "B.rasset", 2, Span<const PackageTagInput>(renamed)));
    REQUIRE(AddPackage(env, project, "C.rasset", 3, Span<const PackageTagInput>(moved)));
    REQUIRE(AddPackage(env, project, "D1.rasset", 4, Span<const PackageTagInput>(duplicate)));
    REQUIRE(AddPackage(env, project, "D2.rasset", 5, Span<const PackageTagInput>(duplicate)));
    REQUIRE(AddPackage(env, project, "E.rasset", 6, Span<const PackageTagInput>(current)));
    REQUIRE(AddPackage(env, project, "F.rasset", 7, Span<const PackageTagInput>(former)));
    REQUIRE(AddPackage(env, project, "G.rasset", 8, Span<const PackageTagInput>(twoFormer)));
    REQUIRE(AddPackage(env, project, "S1.rasset", 9, Span<const PackageTagInput>(same)));
    REQUIRE(AddPackage(env, project, "S2.rasset", 9, Span<const PackageTagInput>(same)));
    // 같은 id의 두 파일 가운데 한쪽에만 태그가 있다.
    REQUIRE(AddPackage(env, project, "T1.rasset", 20, Span<const PackageTagInput>(oneSided)));
    REQUIRE(AddPackage(env, project, "T2.rasset", 20, {}));
    REQUIRE(AddPackage(env, project, "U.rasset", 21, Span<const PackageTagInput>(otherTags)));
    REQUIRE(AddPackage(env, project, "V.rasset", 22, Span<const PackageTagInput>(vTags)));
    REQUIRE(AddPackage(env, project, "W.rasset", 23, Span<const PackageTagInput>(wTags)));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    AssetIndex index(env.allocator, layout);
    REQUIRE(index.Rebuild(platform::PhysicalFileSystem(), PackageReadOptions{}));
    REQUIRE(index.Count() == 15);
    ExchangeIndexLookup lookup(index, env.allocator);

    CHECK(IsFound(lookup.Find("/Game/Tex/T.T", {}), 1, "/Game/A"));
    // 둘째 단계: UE에서 이름을 바꾼 뒤(옛 경로가 아직 지금 경로 태그).
    const StringView renamedFrom[] = {"/Game/Old/Renamed.Renamed"};
    CHECK(IsFound(lookup.Find("/Game/New/Renamed.Renamed", Span<const StringView>(renamedFrom)), 2,
                  "/Game/B", true));
    // 둘째 단계: 옛 경로 목록 태그가 지금 경로와 같다.
    CHECK(IsFound(lookup.Find("/Game/Older/X.X", {}), 3, "/Game/C", true));
    CHECK(IsState(lookup.Find("/Game/Dup/D.D", {}), ExchangeExistingState::Ambiguous));
    // 첫 단계가 있으면 둘째 단계(옛 경로 목록의 F)는 보지 않는다.
    CHECK(IsFound(lookup.Find("/Game/Mixed/M.M", {}), 6, "/Game/E"));
    // 같은 에셋이 옛 경로 둘로 맞는다.
    const StringView gFrom[] = {"/Game/G/Old1.Old1", "/Game/G/Old2.Old2"};
    CHECK(
        IsFound(lookup.Find("/Game/G/New.New", Span<const StringView>(gFrom)), 8, "/Game/G", true));
    CHECK(IsState(lookup.Find("/Game/None.None", {}), ExchangeExistingState::None));
    // 같은 id 두 파일.
    CHECK(IsState(lookup.Find("/Game/Same/S.S", {}), ExchangeExistingState::Ambiguous));
    CHECK(IsState(lookup.Find("/Game/One/O.O", {}), ExchangeExistingState::Ambiguous));
    // 둘째 단계에서 같은 에셋이 다른 키로 다시 맞는다(V는 옛 경로 태그와 지금 경로 태그 둘 다).
    const StringView vFrom[] = {"/Game/V1.V1", "/Game/V3.V3"};
    CHECK(IsState(lookup.Find("/Game/V2.V2", Span<const StringView>(vFrom)),
                  ExchangeExistingState::Ambiguous));
    // 둘째 단계의 서로 다른 두 에셋.
    CHECK(IsState(lookup.Find("/Game/Older/X.X", Span<const StringView>(renamedFrom)),
                  ExchangeExistingState::Ambiguous));

    // 경로 점유: 색인에 있는 경로(대소문자 무시), 없는 경로, 논리 경로 규칙 위반.
    auto taken = lookup.PathTaken("/Game/a");
    CHECK(taken && taken.Value());
    auto vacant = lookup.PathTaken("/Game/Unreal/Game/Tex/T");
    CHECK(vacant && !vacant.Value());
    CHECK(!lookup.PathTaken("Game/A"));

    // 할당 실패: 조회 종류마다(정렬 표 만들기 포함) 모든 지점에서 OutOfMemory, 그 뒤 같은 결과.
    struct Query {
        StringView objectPath;
        Span<const StringView> former;
        uint64 id;
        StringView path;
        bool byFormer;
    };
    const Query queries[] = {
        {"/Game/Tex/T.T", {}, 1, "/Game/A", false},
        {"/Game/New/Renamed.Renamed", Span<const StringView>(renamedFrom), 2, "/Game/B", true},
        {"/Game/Older/X.X", {}, 3, "/Game/C", true},
        {"/Game/G/New.New", Span<const StringView>(gFrom), 8, "/Game/G", true},
    };
    uint32 failures = 0;
    for (const Query& query : queries) {
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(env.allocator);
            failing.FailOnAttempt(attempt);
            ExchangeIndexLookup failingLookup(index, failing);
            auto found = failingLookup.Find(query.objectPath, query.former);
            if (!found) {
                CHECK(found.Error() == ErrorCode::OutOfMemory);
                ++failures;
                continue;
            }

            CHECK(IsFound(found, query.id, query.path, query.byFormer));
            break;
        }
    }

    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        failing.FailOnAttempt(attempt);
        ExchangeIndexLookup failingLookup(index, failing);
        auto pathTaken = failingLookup.PathTaken("/Game/A");
        if (!pathTaken) {
            CHECK(pathTaken.Error() == ErrorCode::OutOfMemory);
            ++failures;
            continue;
        }

        CHECK(pathTaken.Value());
        break;
    }

    CHECK(failures >= 8);
}

} // namespace exchange_index_lookup_tests_file
