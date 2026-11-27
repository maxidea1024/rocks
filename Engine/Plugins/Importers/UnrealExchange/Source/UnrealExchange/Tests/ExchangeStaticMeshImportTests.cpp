#include "ExchangeImportTestSupport.h"

#include <Core/Memory/SystemAllocator.h>

// 교환 정적 메시 임포트 시험이다(Docs/Design/UnrealExchange 5.5절 12번 — UX3b2): 저장소 golden
// 묶음의 정적 메시 둘을 디스크 프로젝트에 asset-import-unreal로 들이고(원본 LOD·슬롯·충돌·태그·
// 기록 옵션), 다시 실행하면 재임포트(id 유지), 일반 asset-reimport, --mesh-normals=rocks, 그리고
// asset-import로 A2f 임포터 unreal_static_mesh의 거부(10.6절 6번 순서·변환 실패)와 경고.
using namespace rocks;
using namespace rocks::asset_database_test;
using namespace rocks::exchange_test;
namespace exchange_static_mesh_import_tests_file {

namespace {

constexpr StringView CubePath = "/Engine/BasicShapes/Cube.Cube";
constexpr StringView DegeneratePath = "/Game/RxGolden/SM_Degenerate.SM_Degenerate";
constexpr StringView CubeAsset = "/Game/Unreal/Engine/BasicShapes/Cube";
constexpr StringView DegenerateAsset = "/Game/Unreal/Game/RxGolden/SM_Degenerate";
constexpr StringView SkeletalCubePath = "/Engine/EngineMeshes/SkeletalCube.SkeletalCube";
// Cube 패키지 파일(프로젝트 안 상대 — /Game은 Content)이다.
constexpr StringView CubePackage = "Content/Unreal/Engine/BasicShapes/Cube.rasset";
// 원본 하나도 빼지 않는다(MakeSourceArgs 인자 목록 그대로).
constexpr usize SkipNone = NumericLimits<usize>::Max();

} // namespace

// asset-import-unreal: golden 정적 메시 둘의 새 임포트(원본 LOD·슬롯·충돌·태그·기록 옵션), 다시
// 실행하면 재임포트, 일반 asset-reimport는 같은 결과, --mesh-normals=rocks는 기록 옵션을 바꾼다.
TEST(UnrealExchangeStaticMesh_ImportCommand) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux3b-command");
        REQUIRE(scene.ready);
        Tool tool(allocator);
        StringBuilder<256> onlyCube(allocator);
        StringBuilder<256> onlyDegenerate(allocator);
        REQUIRE(onlyCube.TryAppendFormat("--only={}", CubePath) &&
                onlyDegenerate.TryAppendFormat("--only={}", DegeneratePath));
        const StringView run[] = {scene.bundle.View(), onlyCube.View(), onlyDegenerate.View(),
                                  scene.project.View()};
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 2 created, 0 reimported, 0 failed, 0 skipped"));
        // Cube(옛 에셋)의 bUseBackwardsCompatibleF16TruncUVs는 Rocks 필드가 없다.
        CHECK(tool.Error("UnsupportedSetting"));

        const StringView cubeInfo[] = {CubeAsset, scene.project.View()};
        CHECK(tool.Info(cubeInfo) == 0);
        // 설정·슬롯(묶음 밖 머티리얼은 비움)·충돌, Unreal 모드: UE가 계산한 원본이라 다시 계산하지
        // 않고 lightmap 번호는 preparedBuild다.
        CHECK(tool.Out("lightMapResolution=64") && tool.Out("lightMapCoordinateIndex=1") &&
              tool.Out("materialSlots[0].slotName=WorldGridMaterial") &&
              tool.Out("materialSlots[0].material=none") &&
              tool.Out("simpleCollision.boxes[0].size.x=100") &&
              tool.Out("sourceLods[0].build.recomputeTangents=false") &&
              tool.Out("sourceLods[0].build.dstLightmapIndex=2") && !tool.Out("sourceLods[1]"));
        // 태그(10.6절 7번): 저장된 패키지 Tags에 ObjectPath 키와 값이 있다.
        CHECK(scene.FileContains(CubePackage, "UnrealExchange.ObjectPath") &&
              scene.FileContains(CubePackage, CubePath));
        const StringView importInfo[] = {CubeAsset, "--import", scene.project.View()};
        CHECK(tool.Info(importInfo) == 0);
        CHECK(tool.Out("import.importer=unreal_static_mesh 1 current") &&
              tool.Out("import.source.asset=") && tool.Out("import.source.blob:"));
        const StringView degenerateInfo[] = {DegenerateAsset, scene.project.View()};
        CHECK(tool.Info(degenerateInfo) == 0);

        // 다시 실행하면 재임포트다(같은 에셋 — 필드가 같다).
        CHECK(tool.Info(cubeInfo) == 0);
        String before(allocator);
        REQUIRE(Keep(tool, before));
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 2 reimported, 0 failed, 0 skipped"));
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        // 재임포트는 설정을 UE 값으로 되돌린다(replaceSettingsOnReimport — 원본 쪽이 정본).
        const StringView edit[] = {CubeAsset, "lightMapResolution=8", scene.project.View()};
        CHECK(tool.Set(edit) == 0);
        CHECK(tool.Info(cubeInfo) == 0 && tool.Out("lightMapResolution=8"));
        const StringView cubeRun[] = {scene.bundle.View(), onlyCube.View(), scene.project.View()};
        CHECK(tool.Unreal(cubeRun) == 0);
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        // 일반 asset-reimport는 같은 결과다(편집한 설정도 되돌린다).
        CHECK(tool.Set(edit) == 0);
        const StringView plain[] = {CubeAsset, scene.project.View()};
        CHECK(tool.Reimport(plain) == 0);
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        // --mesh-normals=rocks: 순수 원본과 UE 빌드 설정 그대로다(Cube는 recomputeTangents 참).
        const StringView rocks[] = {scene.bundle.View(), onlyCube.View(), "--mesh-normals=rocks",
                                    scene.project.View()};
        CHECK(tool.Unreal(rocks) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 1 reimported, 0 failed, 0 skipped"));
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(tool.Out("sourceLods[0].build.recomputeTangents=true") &&
              tool.Out("sourceLods[0].build.dstLightmapIndex=2"));
        // 일반 asset-reimport는 기록 옵션(Rocks)을 다시 쓴다.
        CHECK(tool.Reimport(plain) == 0);
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(tool.Out("sourceLods[0].build.recomputeTangents=true"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// A2f 임포터 unreal_static_mesh의 거부와 경고(asset-import로 golden 원본을 직접 준다).
TEST(UnrealExchangeStaticMesh_ImporterRejects) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux3b-rejects");
        REQUIRE(scene.ready);
        SourceArgs sources(allocator);
        REQUIRE(MakeSourceArgs(allocator, scene.bundle.View(), CubePath, sources));
        REQUIRE(sources.texts.Size() > 2);
        constexpr StringView Importer = "--importer=unreal_static_mesh";
        StringBuilder<256> objectPath(allocator);
        REQUIRE(objectPath.TryAppendFormat("--objectPath={}", CubePath));
        Tool tool(allocator);

        const auto withSources = [&](StringView path, Array<StringView>& args, usize skip) {
            bool ok = args.TryPushBack(path) && args.TryPushBack(scene.project.View());
            for (usize index = 0; index < sources.texts.Size(); ++index) {
                ok = ok && (index == skip || args.TryPushBack(sources.texts[index].View()));
            }

            return ok;
        };

        // 정상: 경고는 Rocks 필드가 없는 설정 하나다.
        {
            Array<StringView> args(allocator);
            REQUIRE(withSources("/Game/X/Good", args, SkipNone) && args.TryPushBack(Importer) &&
                    args.TryPushBack(objectPath.View()));
            CHECK(tool.Import(args.View()) == 0);
            CHECK(tool.Error("UnsupportedSetting"));
        }

        // 다른 클래스 임포터(텍스처), objectPath 불일치, blob 원본 하나 없음.
        struct Case {
            StringView path;
            StringView importer;
            StringView objectPath;
            usize skip;
            StringView code;
        };
        const Case cases[] = {
            {"/Game/X/Texture", "--importer=unreal_texture_2d", objectPath.View(), SkipNone,
             "InvalidSource"},
            {"/Game/X/Path", Importer, "--objectPath=/Game/T/Other.Other", SkipNone,
             "InvalidImportOption"},
            {"/Game/X/NoBlob", Importer, objectPath.View(), 1, "MissingSource"},
        };
        for (const Case& item : cases) {
            Array<StringView> args(allocator);
            REQUIRE(withSources(item.path, args, item.skip) && args.TryPushBack(item.importer) &&
                    args.TryPushBack(item.objectPath));
            CHECK(tool.Import(args.View()) == 1);
            CHECK(tool.Error(item.code));
            if (!tool.Error(item.code)) {
                tool.Print("case");
            }
        }

        // 정적 메시가 아닌 문서(스킨 메시)를 unreal_static_mesh로 주면 클래스 거부다(blob 전에
        // 본다).
        {
            SourceArgs skeletal(allocator);
            REQUIRE(MakeSourceArgs(allocator, scene.bundle.View(), SkeletalCubePath, skeletal));
            StringBuilder<256> skeletalPath(allocator);
            REQUIRE(skeletalPath.TryAppendFormat("--objectPath={}", SkeletalCubePath));
            const StringView args[] = {"/Game/X/Skinned", scene.project.View(),
                                       skeletal.texts[0].View(), Importer, skeletalPath.View()};
            CHECK(tool.Import(args) == 1);
            CHECK(tool.Error("InvalidSource"));
        }

        // 변환 실패: SourceModels 수(빈 LOD 설정을 앞에 하나 더함)와 data.lods 수가 다른
        // 문서(blob은 그대로)다 — 정적 메시 변환의 CorruptBundle이다.
        Array<char> original(allocator);
        REQUIRE(
            ReadRepositoryFile(allocator,
                               "/Plugins/Importers/UnrealExchange/Tests/Bundles/Golden.rxbundle/"
                               "assets/Engine/BasicShapes/Cube.json",
                               original));
        const StringView text(original.Data(), original.Size());
        constexpr StringView Marker = "\"SourceModels\": [";
        const usize at = text.Find(Marker, StringComparison::CaseSensitive);
        REQUIRE(at != StringView::NotFound);
        const usize split = at + Marker.ByteCount();
        StringBuilder<> edited(allocator);
        REQUIRE(edited.TryAppend(text.SliceBytes(0, split)) && edited.TryAppend("{}, ") &&
                edited.TryAppend(text.SliceBytes(split, text.ByteCount() - split)));
        REQUIRE(scene.WriteText("Src/cube.json", edited.View()));
        StringBuilder<1024> document(allocator);
        REQUIRE(document.TryAppendFormat("--source.asset={}/Src/cube.json", scene.root.Root()));
        Array<StringView> args(allocator);
        REQUIRE(args.TryPushBack("/Game/X/Lods") && args.TryPushBack(scene.project.View()) &&
                args.TryPushBack(document.View()) && args.TryPushBack(Importer) &&
                args.TryPushBack(objectPath.View()));
        for (usize index = 1; index < sources.texts.Size(); ++index) {
            REQUIRE(args.TryPushBack(sources.texts[index].View()));
        }

        CHECK(tool.Import(args.View()) == 1);
        CHECK(tool.Error("CorruptBundle"));
        if (!tool.Error("CorruptBundle")) {
            tool.Print("lods");
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_static_mesh_import_tests_file
