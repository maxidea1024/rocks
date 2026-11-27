#include "ExchangeImportTestSupport.h"

#include <Core/Memory/SystemAllocator.h>

// 교환 스켈레톤·스킨 메시 임포트 시험이다(Docs/Design/UnrealExchange 6.4절 10번 — UX7b2): golden
// 묶음의 SK_Column(스킨 메시 — Hard 닫힘에 스켈레톤)을 asset-import-unreal로 들이고(스켈레톤 참조가
// 묶음 안 스켈레톤 — 보조 스켈레톤 출력 없음, 슬롯·LOD 설정·태그), 다시 실행하면 재임포트(편집한
// 설정이 UE 값으로), 일반 asset-reimport, 그리고 asset-import로 A2f 임포터 둘의 클래스 거부.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_skeletal_import_tests_file {

namespace {

constexpr StringView ColumnPath = "/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column.SK_Column";
constexpr StringView ColumnSkeletonPath =
    "/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column_Skeleton.SK_Column_Skeleton";
constexpr StringView ColumnAsset = "/Game/Unreal/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column";
constexpr StringView ColumnSkeletonAsset =
    "/Game/Unreal/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column_Skeleton";
constexpr StringView ColumnPackage =
    "Content/Unreal/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column.rasset";

} // namespace

// asset-import-unreal: 스킨 메시와 그 스켈레톤(Hard 닫힘)의 새 임포트, 재임포트, 일반 재임포트.
TEST(UnrealExchangeSkeletal_ImportCommand) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux7b-command");
        REQUIRE(scene.ready);
        Tool tool(allocator);
        StringBuilder<256> only(allocator);
        REQUIRE(only.TryAppendFormat("--only={}", ColumnPath));
        const StringView run[] = {scene.bundle.View(), only.View(), scene.project.View()};
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 2 created, 0 reimported, 0 failed"));
        // 물리 에셋(범위 밖) 참조는 Rocks 필드가 없다.
        CHECK(tool.Error("UnsupportedSetting"));

        const StringView meshInfo[] = {ColumnAsset, scene.project.View()};
        CHECK(tool.Info(meshInfo) == 0);
        CHECK(tool.Out(ColumnSkeletonAsset) &&
              tool.Out("sourceLods[0].build.recomputeTangents=true") &&
              tool.Out("sourceLods[0].reduction.maxTriangles=4") && !tool.Out("sourceLods[1]"));
        const StringView skeletonInfo[] = {ColumnSkeletonAsset, scene.project.View()};
        CHECK(tool.Info(skeletonInfo) == 0);
        CHECK(tool.Out("Bulge") && tool.Out("Tip"));
        const StringView importInfo[] = {ColumnAsset, "--import", scene.project.View()};
        CHECK(tool.Info(importInfo) == 0 &&
              tool.Out("import.importer=unreal_skeletal_mesh 1 current"));
        CHECK(scene.FileContains(ColumnPackage, "UnrealExchange.ObjectPath") &&
              scene.FileContains(ColumnPackage, ColumnPath));

        // 재임포트(같은 에셋 — 필드가 같다), 편집한 설정은 UE 값으로 되돌린다.
        CHECK(tool.Info(meshInfo) == 0);
        String before(allocator);
        REQUIRE(Keep(tool, before));
        const StringView edit[] = {ColumnAsset, "minLod.defaultValue=1", scene.project.View()};
        CHECK(tool.Set(edit) == 0);
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 2 reimported, 0 failed"));
        CHECK(tool.Info(meshInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        const StringView plain[] = {ColumnAsset, scene.project.View()};
        CHECK(tool.Set(edit) == 0);
        CHECK(tool.Reimport(plain) == 0);
        CHECK(tool.Info(meshInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// A2f 임포터 둘의 클래스 거부와 정상 임포트(asset-import로 golden 원본을 직접 준다).
TEST(UnrealExchangeSkeletal_ImporterRejects) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux7b-rejects");
        REQUIRE(scene.ready);
        SourceArgs skeleton(allocator);
        SourceArgs mesh(allocator);
        REQUIRE(MakeSourceArgs(allocator, scene.bundle.View(), ColumnSkeletonPath, skeleton));
        REQUIRE(MakeSourceArgs(allocator, scene.bundle.View(), ColumnPath, mesh));
        StringBuilder<256> skeletonPath(allocator);
        StringBuilder<256> meshPath(allocator);
        REQUIRE(skeletonPath.TryAppendFormat("--objectPath={}", ColumnSkeletonPath) &&
                meshPath.TryAppendFormat("--objectPath={}", ColumnPath));
        Tool tool(allocator);

        // 정상: 스켈레톤(blob 없음).
        Array<StringView> good(allocator);
        REQUIRE(good.TryPushBack("/Game/X/Skeleton") && good.TryPushBack(scene.project.View()) &&
                good.TryPushBack(skeleton.texts[0].View()) &&
                good.TryPushBack("--importer=unreal_skeleton") &&
                good.TryPushBack(skeletonPath.View()));
        CHECK(tool.Import(good.View()) == 0);

        // 정상: 스킨 메시(스켈레톤 참조 옵션 없음 — 보조 출력 스켈레톤을 만든다).
        Array<StringView> meshArgs(allocator);
        REQUIRE(meshArgs.TryPushBack("/Game/X/Mesh") &&
                meshArgs.TryPushBack(scene.project.View()) &&
                meshArgs.TryPushBack("--importer=unreal_skeletal_mesh") &&
                meshArgs.TryPushBack(meshPath.View()));
        for (const String& text : mesh.texts) {
            REQUIRE(meshArgs.TryPushBack(text.View()));
        }

        CHECK(tool.Import(meshArgs.View()) == 0);
        CHECK(tool.Out("output Skeleton"));

        // 다른 클래스 문서: 스킨 메시 문서를 unreal_skeleton으로, 스켈레톤 문서를
        // unreal_skeletal_mesh로.
        const StringView wrongSkeleton[] = {"/Game/X/WrongSkeleton", scene.project.View(),
                                            mesh.texts[0].View(), "--importer=unreal_skeleton",
                                            meshPath.View()};
        CHECK(tool.Import(wrongSkeleton) == 1 && tool.Error("InvalidSource"));
        const StringView wrongMesh[] = {"/Game/X/WrongMesh", scene.project.View(),
                                        skeleton.texts[0].View(), "--importer=unreal_skeletal_mesh",
                                        skeletonPath.View()};
        CHECK(tool.Import(wrongMesh) == 1 && tool.Error("InvalidSource"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_skeletal_import_tests_file
