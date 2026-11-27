#include "ExchangeMeshTestSupport.h"

#include <Mesh/MeshBuildSettings.h>
#include <Object/ModuleRegistration.h>
#include <Object/TypeRegistry.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeStaticMesh.h>
#include <UnrealExchange/ExchangeStaticMeshSettings.h>
#include <stdio.h>

// 교환 정적 메시 → 임포트 명세 변환 시험이다(Docs/Design/UnrealExchange 5.5절 12번 — UX3b1):
// golden 묶음 정적 메시 둘의 두 모드(원본 선택·빌드 플래그·lightmap 번호), 기록 적재의 형식
// 거부·NFC 정규화와 재정렬, 설정 번역(이름 바꾼 행·Unsupported·모르는 키·PerPlatform·참조), 슬롯·
// 섹션 키 풀기와 정렬·소켓, 충돌 형상 다섯·level set 경고·CollisionTraceFlag. 변형 입력은 golden
// 문서를 경로 편집으로 다시 쓴 사본이다(blob은 golden 그대로 + 시험이 더한 것).
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_static_mesh_tests_file {

namespace {

constexpr StringView CubePath = "/Engine/BasicShapes/Cube.Cube";
constexpr StringView DegeneratePath = "/Game/RxGolden/SM_Degenerate.SM_Degenerate";
constexpr StringView GridMaterialPath =
    "/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial";
// Cube의 UE 준비 원본 lightmap 번호와 lightmap 해상도·좌표 번호다(golden 묶음 값).
constexpr int32 CubePreparedDstLightmap = 2;
constexpr int32 CubeLightMapResolution = 64;
constexpr int32 CubeLightMapCoordinate = 1;
constexpr double CubeBoxSize = 100.0;
constexpr float CubeHardAngle = 45.0f;
constexpr float CubeDistanceFieldScale = 2.0f;
constexpr float CubeUvDensity1 = 356.392792f;
// golden 묶음과 정적 메시 에셋 둘의 blob byte다.
struct Golden {
    explicit Golden(Allocator& allocator) noexcept
        : bundle(allocator), cubeBytes(allocator), cubeBlobs(allocator), degenerateBytes(allocator),
          degenerateBlobs(allocator) {
    }

    ExchangeBundle bundle;
    uint32 cube = ExchangeNoIndex;
    uint32 degenerate = ExchangeNoIndex;
    Array<Array<uint8>> cubeBytes;
    Array<Span<const uint8>> cubeBlobs;
    Array<Array<uint8>> degenerateBytes;
    Array<Span<const uint8>> degenerateBlobs;
};

bool OpenGolden(Allocator& allocator, Golden& golden) noexcept {
    StringBuilder<512> directory(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    if (!FixtureSourcePath("Golden.rxbundle", directory) ||
        !ReadExchangeBundle(platform::PhysicalFileSystem(), directory.View(), {}, golden.bundle,
                            diagnostics)) {
        return false;
    }

    golden.cube = FindAsset(golden.bundle, CubePath);
    golden.degenerate = FindAsset(golden.bundle, DegeneratePath);
    return golden.cube != ExchangeNoIndex && golden.degenerate != ExchangeNoIndex &&
           LoadBlobs(golden.bundle, golden.cube, golden.cubeBytes, golden.cubeBlobs) &&
           LoadBlobs(golden.bundle, golden.degenerate, golden.degenerateBytes,
                     golden.degenerateBlobs);
}

ExchangeStaticMeshInput Input(MeshEnv& env, const ExchangeAsset& asset,
                              Span<const Span<const uint8>> blobs,
                              ExchangeMeshNormals normals = ExchangeMeshNormals::Unreal,
                              const OrderedMap<String, AssetId>* references = nullptr) noexcept {
    ExchangeStaticMeshInput input;
    input.asset = &asset;
    input.blobs = blobs;
    input.normals = normals;
    input.references = references;
    input.registry = &env.objects;
    input.access = env.access;
    return input;
}

// edits를 base에 적용해 변환한다. 결과 오류다(성공이면 None).
struct Attempt {
    explicit Attempt(Allocator& allocator) noexcept : edited(allocator), mesh(allocator) {
    }

    EditedAsset edited;
    ExchangeStaticMesh mesh;
    ExchangeMeshFailure failure;
    ErrorCode error = ErrorCode::None;
    bool prepared = false;
};

void Run(MeshEnv& env, Allocator& allocator, const ExchangeAsset& base,
         Span<const Span<const uint8>> blobs, Span<const JsonEdit> edits, Attempt& attempt,
         ExchangeMeshNormals normals = ExchangeMeshNormals::Unreal,
         const OrderedMap<String, AssetId>* references = nullptr) noexcept {
    attempt.prepared = Edit(allocator, base, blobs, edits, attempt.edited);
    if (!attempt.prepared) {
        return;
    }

    auto built = attempt.mesh.Build(
        Input(env, attempt.edited.asset, attempt.edited.blobs.View(), normals, references),
        attempt.failure);
    attempt.error = built ? ErrorCode::None : built.Error();
}

bool Rejects(MeshEnv& env, Allocator& allocator, const ExchangeAsset& base,
             Span<const Span<const uint8>> blobs, Span<const JsonEdit> edits,
             AssetDiagnosticCode code, ErrorCode error, uint64 detail,
             ExchangeMeshNormals normals = ExchangeMeshNormals::Unreal) noexcept {
    Attempt attempt(allocator);
    Run(env, allocator, base, blobs, edits, attempt, normals);
    const bool ok = attempt.prepared && attempt.error == error && attempt.failure.code == code &&
                    attempt.failure.detail == detail;
    if (!ok) {
        printf("    rejects: prepared %d error %d code %s detail %llu\n", attempt.prepared,
               static_cast<int>(attempt.error), AssetDiagnosticName(attempt.failure.code),
               static_cast<unsigned long long>(attempt.failure.detail));
    }

    return ok;
}

usize CountWarnings(const ExchangeStaticMesh& mesh, AssetDiagnosticCode code, uint64 detail,
                    StringView key = {}) noexcept {
    usize count = 0;
    for (const ExchangeSettingWarning& warning : mesh.Warnings()) {
        count += warning.code == code && warning.detail == detail &&
                         (key.IsEmpty() || warning.key == key)
                     ? 1
                     : 0;
    }

    return count;
}

void PrintWarnings(const ExchangeStaticMesh& mesh) noexcept {
    for (const ExchangeSettingWarning& warning : mesh.Warnings()) {
        printf("    warning %s detail %llu key %.*s\n", AssetDiagnosticName(warning.code),
               static_cast<unsigned long long>(warning.detail),
               static_cast<int>(warning.key.ByteCount()), warning.key.Data());
    }
}

uint64 RowOf(StringView key) noexcept {
    const Span<const ExchangeSettingRow> rows = ExchangeStaticMeshSettingRows();
    for (usize index = 0; index < rows.Size(); ++index) {
        if (rows[index].ueKey == key) {
            return index;
        }
    }

    return ExchangeUnknownSettingDetail;
}

// 기록의 속성 번호(이름 — 첫 것)다. 없으면 attributes 크기다.
usize AttributeIndex(JsonValue record, StringView name) noexcept {
    const JsonValue attributes = record.Find("attributes");
    for (usize index = 0; index < attributes.Size(); ++index) {
        if (attributes.At(index).Find("name").GetString() == name) {
            return index;
        }
    }

    return attributes.Size();
}

} // namespace

// golden 정적 메시 둘: 두 모드 모두 변환되고 LOD·설정·슬롯·섹션·충돌이 덤프 값이다.
TEST(UnrealExchange_StaticMeshGolden) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        const ExchangeAsset& cube = golden.bundle.Assets()[golden.cube];
        OrderedMap<String, AssetId> references(allocator);
        String grid(allocator);
        REQUIRE(grid.TryAssign(GridMaterialPath));
        const AssetId gridId{Guid{0x11, 0x22}};
        REQUIRE(references.Insert(Move(grid), gridId));

        for (const ExchangeMeshNormals normals :
             {ExchangeMeshNormals::Unreal, ExchangeMeshNormals::Rocks}) {
            ExchangeStaticMesh mesh(allocator);
            ExchangeMeshFailure failure;
            REQUIRE(mesh.Build(Input(env, cube, golden.cubeBlobs.View(), normals, &references),
                               failure));
            const StaticMeshImportSpec& spec = mesh.Spec();
            REQUIRE(spec.lods.Size() == 1);
            const StaticMeshImportLod& lod = spec.lods[0];
            REQUIRE(lod.source != nullptr);
            CHECK(lod.source == mesh.LodSource(0) && lod.bakedSource == nullptr);
            CHECK(lod.source->Count(MeshElement::Vertex) == 26 &&
                  lod.source->Count(MeshElement::Triangle) == 48);
            // Unreal: UE가 계산한 값이라 다시 계산하지 않고 lightmap 번호는 preparedBuild다. Rocks:
            // UE 빌드 설정 그대로다(Cube는 recomputeNormals 거짓·recomputeTangents 참).
            const bool unreal = normals == ExchangeMeshNormals::Unreal;
            CHECK(!lod.build.recomputeNormals && lod.build.recomputeTangents == !unreal &&
                  !lod.build.generateLightmapUVs);
            CHECK(lod.build.srcLightmapIndex == 0 &&
                  lod.build.dstLightmapIndex == CubePreparedDstLightmap);
            CHECK(Near(lod.build.distanceFieldResolutionScale, CubeDistanceFieldScale) &&
                  Near(lod.build.buildScale.x, 1.0) && lod.build.useMikkTSpace);
            CHECK(Near(lod.reduction.hardAngleThreshold, CubeHardAngle) &&
                  lod.reduction.maxTriangles == NumericLimits<uint32>::Max() &&
                  lod.reduction.terminationCriterion == MeshReductionTermination::Triangles);
            CHECK(Near(lod.screenSize, 1.0) && lod.screenSizePerPlatform.IsEmpty());

            REQUIRE(spec.slots.Size() == 1);
            CHECK(spec.slots[0].slotName == "WorldGridMaterial" &&
                  spec.slots[0].importedSlotName == "WorldGridMaterial" &&
                  spec.slots[0].material == gridId && spec.slots[0].overlayMaterial == AssetId{});
            CHECK(spec.slots[0].uvDensity.initialized &&
                  Near(spec.slots[0].uvDensity.localUvDensities[1], CubeUvDensity1));
            REQUIRE(spec.sectionInfo.Size() == 1);
            CHECK(spec.sectionInfo[0].lod == 0 && spec.sectionInfo[0].section == 0 &&
                  spec.sectionInfo[0].info.materialIndex == 0 &&
                  spec.sectionInfo[0].info.enableCollision);
            CHECK(spec.sockets.IsEmpty());

            REQUIRE(spec.collision != nullptr);
            REQUIRE(spec.collision->boxes.Size() == 1);
            CHECK(spec.collision->ElementCount() == 1 && spec.convexes.IsEmpty());
            const CollisionBox& box = spec.collision->boxes[0];
            CHECK(Near(box.size.x, CubeBoxSize) && Near(box.size.y, CubeBoxSize) &&
                  Near(box.size.z, CubeBoxSize) &&
                  box.collisionEnabled == CollisionEnabled::QueryAndPhysics &&
                  box.contributeToMass && box.name.IsNone());
            CHECK(spec.complexity == CollisionComplexity::Default && !spec.doubleSidedGeometry);

            CHECK(spec.lightMapResolution == CubeLightMapResolution &&
                  spec.lightMapCoordinateIndex == CubeLightMapCoordinate &&
                  spec.lodGroup.IsEmpty() && spec.autoComputeLodScreenSize && spec.minLod == 0 &&
                  spec.numStreamedLods == StaticMeshGroupStreamedLods &&
                  spec.lodForCollision == 0 && !spec.virtualGeometry.enabled &&
                  spec.complexCollisionMesh == AssetId{} &&
                  spec.distanceFieldReplacementMesh == AssetId{} && spec.replaceSettingsOnReimport);
            // Cube(옛 에셋)의 bUseBackwardsCompatibleF16TruncUVs 참만 Rocks 필드가 없다.
            CHECK(mesh.Warnings().Size() == 1);
            CHECK(CountWarnings(mesh, AssetDiagnosticCode::UnsupportedSetting,
                                RowOf("SourceModels"), "bUseBackwardsCompatibleF16TruncUVs") == 1);
            PrintWarnings(mesh);
        }

        // 퇴화 메시: Nanite가 켜져 있고 경고가 없다. references가 없으면 슬롯 머티리얼은 비운다.
        ExchangeStaticMesh mesh(allocator);
        ExchangeMeshFailure failure;
        REQUIRE(mesh.Build(
            Input(env, golden.bundle.Assets()[golden.degenerate], golden.degenerateBlobs.View()),
            failure));
        CHECK(mesh.Spec().virtualGeometry.enabled && mesh.Warnings().IsEmpty());
        CHECK(mesh.Spec().slots.Size() == 1 && mesh.Spec().slots[0].material == AssetId{});
        REQUIRE(mesh.Spec().lods.Size() == 1);
        CHECK(mesh.Spec().lods[0].source->Count(MeshElement::Edge) == 5);
        PrintWarnings(mesh);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 모드가 고른 기록만 읽는다: Unreal은 prepared(source가 망가져도 된다), Rocks는 source다.
TEST(UnrealExchange_StaticMeshModes) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        const ExchangeAsset& cube = golden.bundle.Assets()[golden.cube];
        const Span<const Span<const uint8>> blobs = golden.cubeBlobs.View();
        const JsonEdit brokenSource[] = {{"data.lods.0.source.counts.vertices", R"("x")"}};
        const JsonEdit brokenPrepared[] = {{"data.lods.0.prepared.counts.vertices", R"("x")"}};
        Attempt unreal(allocator);
        Run(env, allocator, cube, blobs, brokenSource, unreal);
        CHECK(unreal.prepared && unreal.error == ErrorCode::None);
        CHECK(Rejects(env, allocator, cube, blobs, brokenSource, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0, ExchangeMeshNormals::Rocks));
        CHECK(Rejects(env, allocator, cube, blobs, brokenPrepared,
                      AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData, 0));
        Attempt rocks(allocator);
        Run(env, allocator, cube, blobs, brokenPrepared, rocks, ExchangeMeshNormals::Rocks);
        CHECK(rocks.prepared && rocks.error == ErrorCode::None);

        // 원본이 있다는 LOD에 고른 기록이나 preparedBuild가 없으면 형식 위반이다.
        const JsonEdit noPrepared[] = {{"data.lods.0.prepared", "null"}};
        CHECK(Rejects(env, allocator, cube, blobs, noPrepared, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));
        const JsonEdit noBuild[] = {{"data.lods.0.preparedBuild", "null"}};
        CHECK(Rejects(env, allocator, cube, blobs, noBuild, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));
        Attempt rocksNoBuild(allocator);
        Run(env, allocator, cube, blobs, noBuild, rocksNoBuild, ExchangeMeshNormals::Rocks);
        CHECK(rocksNoBuild.error == ErrorCode::None);
        const JsonEdit noFlag[] = {{"data.lods.0.hasSource", ""}};
        CHECK(Rejects(env, allocator, cube, blobs, noFlag, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));

        // 원본 없는 LOD: 명세에 원본이 없다(서비스가 거부한다). 생성 LOD의 baked는 bakedSource다.
        StringBuilder<> record(allocator);
        REQUIRE(ToText(cube.data.Find("lods").At(0).Find("source"), {}, allocator, record));
        const JsonEdit generated[] = {{"data.lods.0.hasSource", "false"},
                                      {"data.lods.0.generated", "true"},
                                      {"data.lods.0.baked", record.View()}};
        Attempt baked(allocator);
        Run(env, allocator, cube, blobs, generated, baked);
        REQUIRE(baked.error == ErrorCode::None);
        CHECK(baked.mesh.Spec().lods[0].source == nullptr &&
              baked.mesh.Spec().lods[0].bakedSource != nullptr &&
              baked.mesh.Spec().lods[0].bakedSource == baked.mesh.LodBakedSource(0));
        // Unreal 모드라도 원본이 없으면 빌드 플래그는 UE 값 그대로다.
        CHECK(baked.mesh.Spec().lods[0].build.recomputeTangents);
        const JsonEdit badBaked[] = {{"data.lods.0.baked", "1"}};
        CHECK(Rejects(env, allocator, cube, blobs, badBaked, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));
        // 생성 LOD가 아니면 baked 기록은 쓰지 않는다.
        const JsonEdit ignored[] = {{"data.lods.0.baked", record.View()}};
        Attempt notGenerated(allocator);
        Run(env, allocator, cube, blobs, ignored, notGenerated);
        CHECK(notGenerated.error == ErrorCode::CorruptData);

        // SourceModels 수와 data.lods 수가 다르다.
        const JsonEdit noLods[] = {{"data.lods", "[]"}};
        CHECK(Rejects(env, allocator, cube, blobs, noLods, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 기록 적재의 형식 거부(5.5절 1번)와 호출 계약.
TEST(UnrealExchange_StaticMeshRecordRejects) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        const ExchangeAsset& base = golden.bundle.Assets()[golden.degenerate];
        const Span<const Span<const uint8>> blobs = golden.degenerateBlobs.View();
        const JsonValue prepared = base.data.Find("lods").At(0).Find("prepared");
        const usize color = AttributeIndex(prepared, "Color");
        const usize position = AttributeIndex(prepared, "Position");
        REQUIRE(color < prepared.Find("attributes").Size());
        REQUIRE(position < prepared.Find("attributes").Size());
        StringBuilder<128> colorName(allocator);
        StringBuilder<128> colorElement(allocator);
        StringBuilder<128> colorType(allocator);
        StringBuilder<128> colorChannels(allocator);
        StringBuilder<128> colorBlob(allocator);
        StringBuilder<128> positionType(allocator);
        REQUIRE(colorName.TryAppendFormat("data.lods.0.prepared.attributes.{}.name", color));
        REQUIRE(colorElement.TryAppendFormat("data.lods.0.prepared.attributes.{}.element", color));
        REQUIRE(colorType.TryAppendFormat("data.lods.0.prepared.attributes.{}.type", color));
        REQUIRE(
            colorChannels.TryAppendFormat("data.lods.0.prepared.attributes.{}.channels", color));
        REQUIRE(colorBlob.TryAppendFormat("data.lods.0.prepared.attributes.{}.data.blob", color));
        REQUIRE(positionType.TryAppendFormat("data.lods.0.prepared.attributes.{}.type", position));
        const usize hard = AttributeIndex(prepared, "IsHard");
        const usize tangent = AttributeIndex(prepared, "Tangent");
        const usize uv = AttributeIndex(prepared, "TexCoord");
        REQUIRE(hard < prepared.Find("attributes").Size() && tangent + 1 == uv);
        StringBuilder<128> edgeType(allocator);
        StringBuilder<128> tangentName(allocator);
        StringBuilder<128> uvName(allocator);
        REQUIRE(edgeType.TryAppendFormat("data.lods.0.prepared.attributes.{}.type", hard));
        REQUIRE(tangentName.TryAppendFormat("data.lods.0.prepared.attributes.{}.name", tangent));
        REQUIRE(uvName.TryAppendFormat("data.lods.0.prepared.attributes.{}.name", uv));

        struct Case {
            JsonEdit edit;
            AssetDiagnosticCode code;
        };

        const Case cases[] = {
            {{"data.lods.0.prepared.counts.edges", ""}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.counts.edges", "-1"}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.counts.edges", "4294967296"},
             AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.counts.extra", "1"}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.counts", "[]"}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.topology.6", ""}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.topology", "{}"}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.topology.0.components", "2"},
             AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.attributes", "{}"}, AssetDiagnosticCode::CorruptBundle},
            {{colorElement.View(), R"("Bogus")"}, AssetDiagnosticCode::CorruptBundle},
            {{colorType.View(), R"("Vec5")"}, AssetDiagnosticCode::CorruptBundle},
            {{colorChannels.View(), "0"}, AssetDiagnosticCode::CorruptBundle},
            {{colorChannels.View(), "256"}, AssetDiagnosticCode::CorruptBundle},
            {{colorName.View(), "7"}, AssetDiagnosticCode::CorruptBundle},
            {{colorBlob.View(), R"("00")"}, AssetDiagnosticCode::CorruptBundle},
            // 배치와 값 타입이 맞지 않는다(Vec3 위치를 Vec2로).
            {{positionType.View(), R"("Vec2")"}, AssetDiagnosticCode::CorruptBundle},
            // 기록 순서 위반(같은 요소 안 같은 이름 — 원문 이름으로 엄격 증가가 아님)이다.
            {{colorName.View(), R"("BinormalSign")"}, AssetDiagnosticCode::CorruptBundle},
            // counts와 blob 원소 수가 다르다(위상 코너 배열·정점 위치·다각형 묶음 슬롯 이름).
            {{"data.lods.0.prepared.counts.corners", "6"}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.counts.vertices", "5"}, AssetDiagnosticCode::CorruptBundle},
            {{"data.lods.0.prepared.counts.polygonGroups", "2"},
             AssetDiagnosticCode::CorruptBundle},
            // 값 타입과 배치가 다르다(u8 Bool 변을 Name으로).
            {{edgeType.View(), R"("Name")"}, AssetDiagnosticCode::CorruptBundle},
        };

        for (const Case& item : cases) {
            Attempt attempt(allocator);
            Run(env, allocator, base, blobs, Span<const JsonEdit>(&item.edit, 1), attempt);
            CHECK(attempt.prepared && attempt.failure.code == item.code &&
                  attempt.error == ErrorCode::CorruptData && attempt.failure.detail == 0);
            if (attempt.error == ErrorCode::None || attempt.failure.code != item.code) {
                printf("    case %.*s: error %d code %s\n",
                       static_cast<int>(item.edit.path.ByteCount()), item.edit.path.Data(),
                       static_cast<int>(attempt.error), AssetDiagnosticName(attempt.failure.code));
            }
        }

        // 정규화 뒤 같은 이름 둘(NFD "e + U+0301"과 NFC "é" — 원문 순서는 맞다)은 Load가 순서
        // 위반으로 거부하고 그 세부가 남는다.
        {
            const JsonEdit collide[] = {{tangentName.View(), "\"e\\u0301\""},
                                        {uvName.View(), "\"\\u00e9\""}};
            Attempt attempt(allocator);
            Run(env, allocator, base, blobs, collide, attempt);
            CHECK(attempt.error == ErrorCode::CorruptData &&
                  attempt.failure.code == AssetDiagnosticCode::CorruptBundle &&
                  attempt.failure.mesh.code == AssetDiagnosticCode::CorruptPackage);
        }

        // 호출 계약: blob 수가 다름·정적 메시가 아님·registry 없음·두 번 부름.
        MeshSource mesh(allocator);
        uint64 normalized = 0;
        ExchangeMeshFailure failure;
        CHECK(LoadExchangeMeshSource(base, {}, prepared, env.names, mesh, normalized, failure)
                      .Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSource);
        CHECK(LoadExchangeMeshSource(base, blobs, JsonValue(), env.names, mesh, normalized, failure)
                      .Error() == ErrorCode::CorruptData &&
              failure.code == AssetDiagnosticCode::CorruptBundle);
        REQUIRE(
            LoadExchangeMeshSource(base, blobs, prepared, env.names, mesh, normalized, failure));
        CHECK(normalized == 0 && mesh.Count(MeshElement::Vertex) == 4);

        ExchangeStaticMesh converted(allocator);
        ExchangeStaticMeshInput input = Input(env, base, blobs);
        input.blobs = {};
        CHECK(converted.Build(input, failure).Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSource);
        input = Input(env, base, blobs);
        input.registry = nullptr;
        CHECK(converted.Build(input, failure).Error() == ErrorCode::InvalidArgument);
        const ExchangeAsset& texture = golden.bundle.Assets()[golden.cube];
        ExchangeAsset other(allocator);
        other.exchangeClass = ExchangeClass::Texture2D;
        other.settings = texture.settings;
        other.data = texture.data;
        CHECK(converted.Build(Input(env, other, {}), failure).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(converted.Build(Input(env, base, blobs), failure));
        CHECK(converted.Build(Input(env, base, blobs), failure).Error() ==
              ErrorCode::InvalidArgument);
        // 실패한 Build 뒤에도 다시 부를 수 없다.
        Attempt broken(allocator);
        const JsonEdit noRecord[] = {{"data.lods.0.prepared", "null"}};
        Run(env, allocator, base, blobs, noRecord, broken);
        REQUIRE(broken.error == ErrorCode::CorruptData);
        CHECK(broken.mesh.Build(Input(env, base, blobs), failure).Error() ==
                  ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSource);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 이름 NFC 정규화: 속성 이름과 Name 값을 바꾸고 센다. 바뀐 이름이 9.2절 순서를 바꾸면 다시
// 정렬한다(정렬하지 않으면 Load가 순서 위반으로 거부한다).
TEST(UnrealExchange_StaticMeshNameNormalization) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        const ExchangeAsset& base = golden.bundle.Assets()[golden.degenerate];
        const JsonValue source = base.data.Find("lods").At(0).Find("source");
        const usize tangent = AttributeIndex(source, "Tangent");
        const usize uv = AttributeIndex(source, "TexCoord");
        const usize slot = AttributeIndex(source, "MaterialSlotName");
        REQUIRE(tangent + 1 == uv);
        REQUIRE(slot < source.Find("attributes").Size());
        StringBuilder<128> tangentName(allocator);
        StringBuilder<128> uvName(allocator);
        StringBuilder<128> slotData(allocator);
        REQUIRE(tangentName.TryAppendFormat("data.lods.0.source.attributes.{}.name", tangent));
        REQUIRE(uvName.TryAppendFormat("data.lods.0.source.attributes.{}.name", uv));
        REQUIRE(slotData.TryAppendFormat("data.lods.0.source.attributes.{}.data", slot));

        // NFD "e + U+0301"(65 CC 81)은 "f"(66)보다 앞이고 NFC "é"(C3 A9)는 뒤다.
        const JsonEdit renamed[] = {{tangentName.View(), "\"e\\u0301\""},
                                    {uvName.View(), R"("f")"}};
        Attempt attempt(allocator);
        Run(env, allocator, base, golden.degenerateBlobs.View(), renamed, attempt,
            ExchangeMeshNormals::Rocks);
        REQUIRE(attempt.error == ErrorCode::None);
        const MeshSource* mesh = attempt.mesh.LodSource(0);
        REQUIRE(mesh != nullptr);
        const uint32 composed = mesh->FindAttribute(MeshElement::Corner, "\xC3\xA9");
        const uint32 plain = mesh->FindAttribute(MeshElement::Corner, "f");
        CHECK(composed != MeshNoAttribute && plain != MeshNoAttribute && plain < composed);
        CHECK(mesh->FindAttribute(MeshElement::Corner, "e\xCC\x81") == MeshNoAttribute);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnrealNameNormalized, 1) == 1);

        // Name 값(다각형 묶음 슬롯 이름)도 NFC로 바꾼다: 바뀐 것은 속성 이름 하나(Tangent) + 값
        // 하나.
        const uint8 name[] = {6, 0, 0, 0, 'C', 'a', 'f', 'e', 0xCC, 0x81};
        ExchangeMeshFailure failure;
        {
            EditedAsset holder(allocator);
            REQUIRE(Edit(allocator, base, golden.degenerateBlobs.View(), {}, holder));
            StringBuilder<> reference(allocator);
            REQUIRE(AddBlob(holder, Span<const uint8>(name), ExchangeBlobLayout::Names, 1, 1,
                            reference));
            const JsonEdit slotEdits[] = {{tangentName.View(), "\"e\\u0301\""},
                                          {uvName.View(), R"("f")"},
                                          {slotData.View(), reference.View()}};
            EditedAsset target(allocator);
            REQUIRE(Edit(allocator, base, golden.degenerateBlobs.View(), slotEdits, target));
            REQUIRE(target.asset.blobs.TryPushBack(holder.asset.blobs.Last()));
            REQUIRE(target.blobs.TryPushBack(holder.blobs.Last()));
            ExchangeStaticMesh normalized(allocator);
            REQUIRE(normalized.Build(
                Input(env, target.asset, target.blobs.View(), ExchangeMeshNormals::Rocks),
                failure));
            CHECK(CountWarnings(normalized, AssetDiagnosticCode::UnrealNameNormalized, 2) == 1);
        }

        // Name 값 배치가 깨졌다(길이가 bulk를 넘음·남는 byte·잘못된 UTF-8).
        const uint8 longName[] = {9, 0, 0, 0, 'C', 'a', 'f', 'e'};
        const uint8 trailing[] = {1, 0, 0, 0, 'C', 'x'};
        const uint8 shortWord[] = {1, 0};
        const uint8 invalid[] = {1, 0, 0, 0, 0xFF};
        const Span<const uint8> broken[] = {
            Span<const uint8>(longName), Span<const uint8>(trailing), Span<const uint8>(shortWord),
            Span<const uint8>(invalid)};
        for (const Span<const uint8> bytes : broken) {
            EditedAsset holder(allocator);
            REQUIRE(Edit(allocator, base, golden.degenerateBlobs.View(), {}, holder));
            StringBuilder<> brokenReference(allocator);
            REQUIRE(AddBlob(holder, bytes, ExchangeBlobLayout::Names, 1, 1, brokenReference));
            const JsonEdit brokenEdits[] = {{slotData.View(), brokenReference.View()}};
            EditedAsset target(allocator);
            REQUIRE(Edit(allocator, base, golden.degenerateBlobs.View(), brokenEdits, target));
            REQUIRE(target.asset.blobs.TryPushBack(holder.asset.blobs.Last()));
            REQUIRE(target.blobs.TryPushBack(holder.blobs.Last()));
            ExchangeStaticMesh rejected(allocator);
            CHECK(rejected.Build(Input(env, target.asset, target.blobs.View(),
                                       ExchangeMeshNormals::Rocks),
                                 failure)
                          .Error() == ErrorCode::CorruptData &&
                  failure.code == AssetDiagnosticCode::CorruptBundle && failure.detail == 0);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 설정 번역: 이름 바꾼 행, enum, PerPlatform, Unsupported·모르는 키, 참조, 값 형식 거부.
TEST(UnrealExchange_StaticMeshSettings) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        const ExchangeAsset& cube = golden.bundle.Assets()[golden.cube];
        const Span<const Span<const uint8>> blobs = golden.cubeBlobs.View();
        OrderedMap<String, AssetId> references(allocator);
        const AssetId fieldId{Guid{0x31, 0x32}};
        const AssetId complexId{Guid{0x41, 0x42}};
        String field(allocator);
        String complex(allocator);
        REQUIRE(field.TryAssign("/Game/M/SM_Field.SM_Field"));
        REQUIRE(complex.TryAssign("/Game/M/SM_Complex.SM_Complex"));
        REQUIRE(references.Insert(Move(field), fieldId));
        REQUIRE(references.Insert(Move(complex), complexId));

        const JsonEdit edits[] = {
            {"settings.SourceModels.0.BuildSettings.BuildScale3D", R"({"X": 2, "Y": 3, "Z": 4})"},
            {"settings.SourceModels.0.BuildSettings.bUseBackwardsCompatibleF16TruncUVs", "false"},
            {"settings.SourceModels.0.BuildSettings.DistanceFieldReplacementMesh",
             R"({"objectPath": "/Game/M/SM_Field.SM_Field"})"},
            {"settings.SourceModels.0.ReductionSettings.MaxNumOfTriangles", "1000"},
            {"settings.SourceModels.0.ReductionSettings.MaxNumOfVerts", "500"},
            {"settings.SourceModels.0.ReductionSettings.BaseLODModel", "1"},
            {"settings.SourceModels.0.ReductionSettings.TerminationCriterion", R"("Any")"},
            {"settings.SourceModels.0.ReductionSettings.SilhouetteImportance", R"("Highest")"},
            {"settings.SourceModels.0.ScreenSize",
             R"({"Default": 0.5, "PerPlatform": [{"key": "Mobile", "value": 0.25}]})"},
            {"settings.bUnknownThing", "1"},
            {"settings.NeverStream", "true"},
            {"settings.MinLOD",
             R"({"Default": 2, "PerPlatform": [{"key": "Mobile", "value": 1}]})"},
            {"settings.NumStreamedLODs", R"({"Default": 3, "PerPlatform": []})"},
            {"settings.LODGroup", R"("SmallProp")"},
            {"settings.LightMapResolution", "128"},
            {"settings.PositiveBoundsExtension", R"({"X": 1, "Y": 2, "Z": 3})"},
            {"settings.bAutoComputeLODScreenSize", "false"},
            {"settings.LODForCollision", "0"},
            {"settings.NaniteSettings.bEnabled", "true"},
            {"settings.NaniteSettings.KeepPercentTriangles", "0.5"},
            {"settings.NaniteSettings.bVoxelNDF", "false"},
            {"settings.NaniteSettings.ShapePreservation", R"("Voxelize")"},
            {"settings.NaniteSettings.GenerateFallback", R"("Enabled")"},
            {"settings.NaniteSettings.FallbackTarget", R"("RelativeError")"},
            {"settings.NaniteSettings.NormalPrecision", "5"},
            {"settings.NaniteSettings.TargetMinimumResidencyInKB", "64"},
            {"settings.NaniteSettings.DisplacementUVChannel", "2"},
            {"settings.NaniteSettings.NaniteAssemblyData", R"({"Parts": [1], "Nodes": []})"},
            {"settings.ComplexCollisionMesh", R"({"objectPath": "/Game/M/SM_Complex.SM_Complex"})"},
        };
        Attempt attempt(allocator);
        Run(env, allocator, cube, blobs, edits, attempt, ExchangeMeshNormals::Rocks, &references);
        REQUIRE(attempt.error == ErrorCode::None);
        const StaticMeshImportSpec& spec = attempt.mesh.Spec();
        const StaticMeshImportLod& lod = spec.lods[0];
        CHECK(Near(lod.build.buildScale.x, 2.0) && Near(lod.build.buildScale.y, 3.0) &&
              Near(lod.build.buildScale.z, 4.0));
        CHECK(lod.reduction.maxTriangles == 1000 && lod.reduction.maxVertices == 500 &&
              lod.reduction.baseLod == 1 &&
              lod.reduction.terminationCriterion == MeshReductionTermination::Any &&
              lod.reduction.silhouetteImportance == MeshFeatureImportance::Highest);
        CHECK(Near(lod.screenSize, 0.5) && lod.screenSizePerPlatform.IsEmpty());
        CHECK(spec.minLod == 2 && spec.numStreamedLods == 3 && spec.lodGroup == "SmallProp" &&
              spec.lightMapResolution == 128 && Near(spec.positiveBoundsExtension.z, 3.0) &&
              !spec.autoComputeLodScreenSize && spec.virtualGeometry.enabled &&
              Near(spec.virtualGeometry.keepPercentTriangles, 0.5));
        const VirtualGeometrySettings& nanite = spec.virtualGeometry;
        CHECK(!nanite.voxelNdf &&
              nanite.shapePreservation == VirtualGeometryShapePreservation::Voxelize &&
              nanite.generateFallback == VirtualGeometryFallbackMode::Enabled &&
              nanite.fallbackTarget == VirtualGeometryFallbackTarget::RelativeError &&
              nanite.normalPrecision == 5 && nanite.targetMinimumResidencyInKb == 64 &&
              nanite.displacementUvChannel == 2);
        CHECK(spec.distanceFieldReplacementMesh == fieldId &&
              spec.complexCollisionMesh == complexId);
        // 경고: PerPlatform 화면 크기(SourceModels 행)·최소 LOD, 모르는 키, NeverStream, Nanite
        // 조립 데이터.
        CHECK(attempt.mesh.Warnings().Size() == 5);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting, RowOf("MinLOD"),
                            "MinLOD") == 1);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            RowOf("SourceModels"), "ScreenSize") == 1);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            ExchangeUnknownSettingDetail, "bUnknownThing") == 1);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            RowOf("NeverStream"), "NeverStream") == 1);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            RowOf("NaniteSettings"), "NaniteAssemblyData") == 1);
        PrintWarnings(attempt.mesh);

        // references에 없는 참조는 비운다.
        Attempt unresolved(allocator);
        Run(env, allocator, cube, blobs, edits, unresolved);
        REQUIRE(unresolved.error == ErrorCode::None);
        CHECK(unresolved.mesh.Spec().complexCollisionMesh == AssetId{} &&
              unresolved.mesh.Spec().distanceFieldReplacementMesh == AssetId{});

        // LOD 0이 아닌 거리장 대체 메시는 에셋 칸이 없어 경고다(LOD 두 개 — 0번을 복제).
        StringBuilder<> model(allocator);
        StringBuilder<> lodText(allocator);
        const JsonEdit modelEdits[] = {{"BuildSettings.DistanceFieldReplacementMesh",
                                        R"({"objectPath": "/Game/M/SM_Field.SM_Field"})"}};
        REQUIRE(ToText(cube.settings.Find("SourceModels").At(0), modelEdits, allocator, model));
        REQUIRE(ToText(cube.data.Find("lods").At(0), {}, allocator, lodText));
        const JsonEdit twoLods[] = {{"settings.SourceModels.+", model.View()},
                                    {"data.lods.+", lodText.View()}};
        Attempt two(allocator);
        Run(env, allocator, cube, blobs, twoLods, two, ExchangeMeshNormals::Unreal, &references);
        REQUIRE(two.error == ErrorCode::None);
        CHECK(two.mesh.Spec().lods.Size() == 2 && two.mesh.LodSource(1) != nullptr &&
              two.mesh.Spec().distanceFieldReplacementMesh == AssetId{});
        CHECK(CountWarnings(two.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            RowOf("SourceModels"), "DistanceFieldReplacementMesh") == 1);
        const JsonEdit oneModel[] = {{"settings.SourceModels.+", model.View()}};
        CHECK(Rejects(env, allocator, cube, blobs, oneModel, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));

        // 값 형식: ImportJson 거부는 InvalidSource / CorruptData(detail = LOD·행), 코드가 읽는
        // 값의 종류 위반은 CorruptBundle(detail = 행), 번역 형식(참조) 위반은 InvalidSource.
        const JsonEdit importBad[] = {
            {"settings.SourceModels.0.BuildSettings.MinLightmapResolution", R"("big")"}};
        CHECK(Rejects(env, allocator, cube, blobs, importBad, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, 0));
        const JsonEdit enumBad[] = {
            {"settings.SourceModels.0.ReductionSettings.TerminationCriterion", R"("Never")"}};
        CHECK(Rejects(env, allocator, cube, blobs, enumBad, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, 0));
        const JsonEdit naniteBad[] = {{"settings.NaniteSettings.NumRays", R"("many")"}};
        CHECK(Rejects(env, allocator, cube, blobs, naniteBad, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, RowOf("NaniteSettings")));
        const struct {
            JsonEdit edit;
            StringView key;
        } scalars[] = {
            {{"settings.LightMapResolution", R"("x")"}, "LightMapResolution"},
            {{"settings.LightMapCoordinateIndex", "1.5"}, "LightMapCoordinateIndex"},
            {{"settings.PositiveBoundsExtension", R"({"X": 1})"}, "PositiveBoundsExtension"},
            {{"settings.NegativeBoundsExtension", "0"}, "NegativeBoundsExtension"},
            {{"settings.bAutoComputeLODScreenSize", "1"}, "bAutoComputeLODScreenSize"},
            {{"settings.LODGroup", "1"}, "LODGroup"},
            {{"settings.MinLOD", R"({"Default": 0.5, "PerPlatform": []})"}, "MinLOD"},
            {{"settings.NumStreamedLODs", R"({"Default": 4294967296, "PerPlatform": []})"},
             "NumStreamedLODs"},
            {{"settings.LODForCollision", "true"}, "LODForCollision"},
        };
        for (const auto& item : scalars) {
            CHECK(Rejects(env, allocator, cube, blobs, Span<const JsonEdit>(&item.edit, 1),
                          AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData,
                          RowOf(item.key)));
        }

        const JsonEdit screenBad[] = {
            {"settings.SourceModels.0.ScreenSize", R"({"Default": "x", "PerPlatform": []})"}};
        CHECK(Rejects(env, allocator, cube, blobs, screenBad, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, 0));
        const JsonEdit referenceBad[] = {{"settings.ComplexCollisionMesh", "1"}};
        CHECK(Rejects(env, allocator, cube, blobs, referenceBad, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, 0));
        const JsonEdit fieldBad[] = {
            {"settings.SourceModels.0.BuildSettings.DistanceFieldReplacementMesh", "1"}};
        CHECK(Rejects(env, allocator, cube, blobs, fieldBad, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));
        const JsonEdit fieldExtra[] = {
            {"settings.SourceModels.0.BuildSettings.DistanceFieldReplacementMesh",
             R"({"objectPath": "/Game/M/SM_Field.SM_Field", "extra": 1})"}};
        CHECK(Rejects(env, allocator, cube, blobs, fieldExtra, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, 0));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 슬롯·섹션 정보·소켓(5.5절 6–8번).
TEST(UnrealExchange_StaticMeshSlotsSectionsSockets) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        const ExchangeAsset& cube = golden.bundle.Assets()[golden.cube];
        const Span<const Span<const uint8>> blobs = golden.cubeBlobs.View();
        OrderedMap<String, AssetId> references(allocator);
        const AssetId materialId{Guid{0x51, 0x52}};
        String material(allocator);
        REQUIRE(material.TryAssign("/Game/M/M_A.M_A"));
        REQUIRE(references.Insert(Move(material), materialId));

        constexpr StringView Slots =
            R"([{"MaterialInterface": {"objectPath": "/Game/M/M_A.M_A"}, "MaterialSlotName": "A",)"
            R"( "ImportedMaterialSlotName": "None", "UVChannelData": {"bInitialized": false,)"
            R"( "bOverrideDensities": true, "LocalUVDensities": [1, 2, 3, 4]},)"
            R"( "OverlayMaterialInterface": {"objectPath": "/Game/M/M_Missing.M_Missing"}},)"
            R"( {"MaterialInterface": null, "MaterialSlotName": "None",)"
            R"( "ImportedMaterialSlotName": "B", "UVChannelData": {"bInitialized": true,)"
            R"( "bOverrideDensities": false, "LocalUVDensities": [0, 0, 0, 0]},)"
            R"( "OverlayMaterialInterface": {"objectPath": "/Game/M/M_A.M_A"}}])";
        // 키 순서가 숫자 순이 아니다: (1, 1), (0, 1), (0, 0).
        constexpr StringView Sections =
            R"({"Map": [{"key": 65537, "value": {"MaterialIndex": 1, "bEnableCollision": false}},)"
            R"( {"key": 1, "value": {"MaterialIndex": 1, "bCastShadow": false}},)"
            R"( {"key": 0, "value": {"MaterialIndex": 0, "bForceOpaque": true,)"
            R"( "bVisibleInRayTracing": false, "bAffectDistanceFieldLighting": false}}]})";
        constexpr StringView Sockets =
            R"([{"subobject": "StaticMeshSocket_0", "class": "/Script/Engine.StaticMeshSocket",)"
            R"( "properties": {"SocketName": "Grip", "RelativeLocation": {"X": 1, "Y": 2, "Z": 3},)"
            R"( "RelativeRotation": {"Pitch": 10, "Yaw": 20, "Roll": 30},)"
            R"( "RelativeScale": {"X": 1, "Y": 1, "Z": 2}, "Tag": "hand",)"
            R"( "bSocketCreatedAtImport": true, "PreviewStaticMesh": null, "Extra": 1}},)"
            R"( {"subobject": "StaticMeshSocket_1", "class": "/Script/Engine.StaticMeshSocket",)"
            R"( "properties": {"SocketName": "Top"}},)"
            R"( {"subobject": "StaticMeshSocket_2", "class": "/Script/Engine.StaticMeshSocket",)"
            R"( "properties": {"SocketName": "None"}}])";
        const JsonEdit edits[] = {{"settings.StaticMaterials", Slots},
                                  {"settings.SectionInfoMap", Sections},
                                  {"settings.Sockets", Sockets}};
        Attempt attempt(allocator);
        Run(env, allocator, cube, blobs, edits, attempt, ExchangeMeshNormals::Unreal, &references);
        REQUIRE(attempt.error == ErrorCode::None);
        const StaticMeshImportSpec& spec = attempt.mesh.Spec();
        REQUIRE(spec.slots.Size() == 2);
        CHECK(spec.slots[0].slotName == "A" && spec.slots[0].importedSlotName.IsEmpty() &&
              spec.slots[0].material == materialId && spec.slots[0].overlayMaterial == AssetId{});
        CHECK(!spec.slots[0].uvDensity.initialized && spec.slots[0].uvDensity.overrideDensities &&
              Near(spec.slots[0].uvDensity.localUvDensities[3], 4.0));
        CHECK(spec.slots[1].slotName.IsEmpty() && spec.slots[1].importedSlotName == "B" &&
              spec.slots[1].material == AssetId{} && spec.slots[1].overlayMaterial == materialId);
        REQUIRE(spec.sectionInfo.Size() == 3);
        CHECK(spec.sectionInfo[0].lod == 0 && spec.sectionInfo[0].section == 0 &&
              spec.sectionInfo[0].info.forceOpaque &&
              !spec.sectionInfo[0].info.visibleInRayTracing &&
              !spec.sectionInfo[0].info.affectDistanceFieldLighting);
        CHECK(spec.sectionInfo[1].lod == 0 && spec.sectionInfo[1].section == 1 &&
              !spec.sectionInfo[1].info.castShadow && spec.sectionInfo[1].info.materialIndex == 1);
        CHECK(spec.sectionInfo[2].lod == 1 && spec.sectionInfo[2].section == 1 &&
              !spec.sectionInfo[2].info.enableCollision);
        REQUIRE(spec.sockets.Size() == 3);
        CHECK(spec.sockets[2].name.IsEmpty());
        const StaticMeshImportSocket& grip = spec.sockets[0];
        CHECK(grip.name == "Grip" && Near(grip.relativeLocation.y, 2.0) &&
              Near(grip.relativeRotation.pitch, 10.0) && Near(grip.relativeRotation.roll, 30.0) &&
              Near(grip.relativeScale.z, 2.0) && grip.tag == "hand" && grip.createdAtImport);
        // 덤프에 없는 칸은 UE 기본값이다(편집기에서 만든 소켓).
        const StaticMeshImportSocket& top = spec.sockets[1];
        CHECK(top.name == "Top" && Near(top.relativeScale.x, 1.0) && top.tag.IsEmpty() &&
              !top.createdAtImport);
        // 소켓의 모르는 프로퍼티는 Sockets 행 경고다.
        CHECK(attempt.mesh.Warnings().Size() == 2);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting, RowOf("Sockets"),
                            "Extra") == 1);

        const JsonEdit rejects[] = {
            // 같은 (lod, section) 두 번이다.
            {"settings.SectionInfoMap",
             R"({"Map": [{"key": 1, "value": {}}, {"key": 1, "value": {}}]})"},
            {"settings.SectionInfoMap", R"({"Map": [{"key": 4294967296, "value": {}}]})"},
            {"settings.SectionInfoMap", R"({"Map": [{"key": -1, "value": {}}]})"},
        };
        for (const JsonEdit& edit : rejects) {
            CHECK(Rejects(env, allocator, cube, blobs, Span<const JsonEdit>(&edit, 1),
                          AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData,
                          RowOf("SectionInfoMap")));
        }

        const JsonEdit sectionValue[] = {
            {"settings.SectionInfoMap",
             R"({"Map": [{"key": 1, "value": {"MaterialIndex": "a"}}]})"}};
        CHECK(Rejects(env, allocator, cube, blobs, sectionValue, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, RowOf("SectionInfoMap")));
        // 같은 하위 객체를 다시 가리키는 소켓(공유)·종류가 틀린 값이다.
        const JsonEdit shared[] = {
            {"settings.Sockets", R"([{"subobject": "StaticMeshSocket_0"}])"}};
        CHECK(Rejects(env, allocator, cube, blobs, shared, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, RowOf("Sockets")));
        const JsonEdit socketBad[] = {
            {"settings.Sockets",
             R"([{"subobject": "S", "properties": {"RelativeLocation": {"X": 1, "Y": 2}}}])"}};
        CHECK(Rejects(env, allocator, cube, blobs, socketBad, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, RowOf("Sockets")));
        const JsonEdit slotBad[] = {{"settings.StaticMaterials", R"([{"MaterialSlotName": 1}])"}};
        CHECK(Rejects(env, allocator, cube, blobs, slotBad, AssetDiagnosticCode::CorruptBundle,
                      ErrorCode::CorruptData, RowOf("StaticMaterials")));
        // 번역 표의 종류와 맞지 않는 값(배열이어야 할 자리)은 번역 실패다.
        const JsonEdit mapNotArray[] = {{"settings.SectionInfoMap", R"({"Map": {}})"}};
        CHECK(Rejects(env, allocator, cube, blobs, mapNotArray, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, 0));
        const JsonEdit slotsNotArray[] = {{"settings.StaticMaterials", "{}"}};
        CHECK(Rejects(env, allocator, cube, blobs, slotsNotArray,
                      AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData, 0));
        const JsonEdit uvBad[] = {
            {"settings.StaticMaterials", R"([{"UVChannelData": {"LocalUVDensities": [1]}}])"}};
        CHECK(Rejects(env, allocator, cube, blobs, uvBad, AssetDiagnosticCode::InvalidSource,
                      ErrorCode::CorruptData, RowOf("StaticMaterials")));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 충돌(5.5절 9번): 형상 다섯, 받지 않는 형상·값 경고, CollisionTraceFlag, 형식 거부.
TEST(UnrealExchange_StaticMeshCollision) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        const ExchangeAsset& cube = golden.bundle.Assets()[golden.cube];
        const Span<const Span<const uint8>> blobs = golden.cubeBlobs.View();
        constexpr StringView Geometry =
            R"({"SphereElems": [{"Center": {"X": 1, "Y": 0, "Z": 0}, "Radius": 5, "Name": "Ball",)"
            R"( "bContributeToMass": false, "CollisionEnabled": "QueryOnly", "RestOffset": 0.5}],)"
            R"( "BoxElems": [{"Center": {"X": 0, "Y": 0, "Z": 0},)"
            R"( "Rotation": {"Pitch": 0, "Yaw": 45, "Roll": 0}, "X": 2, "Y": 4, "Z": 6}],)"
            R"( "SphylElems": [{"Radius": 3, "Length": 10, "CollisionEnabled": "NoCollision"}],)"
            R"( "TaperedCapsuleElems": [{"Radius0": 1, "Radius1": 2, "Length": 3, "Width": 1,)"
            R"( "bOneSidedCollision": false}, {"Radius0": 1, "Radius1": 1, "Length": 1,)"
            R"( "bOneSidedCollision": true}, {"Radius0": 1, "Radius1": 1, "Length": 1, "Width": 2,)"
            R"( "bOneSidedCollision": true}],)"
            R"( "ConvexElems": [{"VertexData": [{"X": 0, "Y": 0, "Z": 0}, {"X": 1, "Y": 0, "Z": 0},)"
            R"( {"X": 0, "Y": 1, "Z": 0}, {"X": 0, "Y": 0, "Z": 1}], "IndexData": [0, 1, 2, 0, 3, 1],)"
            R"( "ElemBox": {}, "Transform": {"Rotation": {"X": 0, "Y": 0, "Z": 0, "W": 1},)"
            R"( "Translation": {"X": 7, "Y": 8, "Z": 9}, "Scale3D": {"X": 1, "Y": 2, "Z": 1}}}],)"
            R"( "LevelSetElems": [{}, {}], "SkinnedLevelSetElems": [], "MLLevelSetElems": [],)"
            R"( "SkinnedTriangleMeshElems": [{}]})";
        const JsonEdit edits[] = {
            {"data.collision.AggGeom", Geometry},
            {"data.collision.CollisionTraceFlag", R"("CTF_UseComplexAsSimple")"},
            {"data.collision.bDoubleSidedGeometry", "true"}};
        Attempt attempt(allocator);
        Run(env, allocator, cube, blobs, edits, attempt);
        REQUIRE(attempt.error == ErrorCode::None);
        const StaticMeshImportSpec& spec = attempt.mesh.Spec();
        REQUIRE(spec.collision != nullptr);
        const SimpleCollision& collision = *spec.collision;
        REQUIRE(collision.spheres.Size() == 1 && collision.boxes.Size() == 1 &&
                collision.capsules.Size() == 1 && collision.taperedCapsules.Size() == 3 &&
                collision.convexes.Size() == 1 && collision.cylinders.IsEmpty());
        const CollisionSphere& sphere = collision.spheres[0];
        CHECK(Near(sphere.center.x, 1.0) && Near(sphere.radius, 5.0) && !sphere.contributeToMass &&
              sphere.collisionEnabled == CollisionEnabled::QueryOnly &&
              Near(sphere.restOffset, 0.5) && !sphere.name.IsNone());
        CHECK(Near(collision.boxes[0].size.y, 4.0) && Near(collision.boxes[0].rotation.yaw, 45.0));
        CHECK(Near(collision.capsules[0].length, 10.0) &&
              collision.capsules[0].collisionEnabled == CollisionEnabled::NoCollision);
        CHECK(Near(collision.taperedCapsules[0].radius1, 2.0));
        REQUIRE(spec.convexes.Size() == 1);
        CHECK(spec.convexes[0].vertices.Size() == 4 && spec.convexes[0].indices.Size() == 6 &&
              spec.convexes[0].indices[4] == 3 && Near(spec.convexes[0].vertices[3].z, 1.0));
        CHECK(Near(collision.convexes[0].transform.translation.y, 8.0) &&
              Near(collision.convexes[0].transform.scale.y, 2.0) &&
              Near(collision.convexes[0].transform.rotation.w, 1.0));
        CHECK(spec.complexity == CollisionComplexity::ComplexAsSimple && spec.doubleSidedGeometry);
        // 경고: level set·skinned triangle mesh(버림), 테이퍼드 캡슐 Width·한쪽 충돌(값을 버림) —
        // 여러 개여도 종류마다 하나다.
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedCollisionShape,
                            static_cast<uint64>(UnsupportedCollisionFeature::LevelSet)) == 1);
        CHECK(CountWarnings(
                  attempt.mesh, AssetDiagnosticCode::UnsupportedCollisionShape,
                  static_cast<uint64>(UnsupportedCollisionFeature::SkinnedTriangleMesh)) == 1);
        CHECK(CountWarnings(
                  attempt.mesh, AssetDiagnosticCode::UnsupportedCollisionShape,
                  static_cast<uint64>(UnsupportedCollisionFeature::TaperedCapsuleWidth)) == 1);
        CHECK(CountWarnings(
                  attempt.mesh, AssetDiagnosticCode::UnsupportedCollisionShape,
                  static_cast<uint64>(UnsupportedCollisionFeature::TaperedCapsuleOneSided)) == 1);
        CHECK(attempt.mesh.Warnings().Size() == 5);
        PrintWarnings(attempt.mesh);

        // 충돌이 없다(null).
        const JsonEdit none[] = {{"data.collision", "null"}};
        Attempt empty(allocator);
        Run(env, allocator, cube, blobs, none, empty);
        REQUIRE(empty.error == ErrorCode::None);
        CHECK(empty.mesh.Spec().collision == nullptr &&
              empty.mesh.Spec().complexity == CollisionComplexity::Default);

        const struct {
            JsonEdit edit;
            uint64 detail;
        } rejects[] = {
            {{"data.collision", "1"}, 0},
            {{"data.collision.AggGeom", "[]"}, 0},
            {{"data.collision.AggGeom.BoxElems", "{}"}, 0},
            {{"data.collision.AggGeom.LevelSetElems", "{}"}, 0},
            {{"data.collision.CollisionTraceFlag", R"("CTF_Bogus")"}, 0},
            {{"data.collision.bDoubleSidedGeometry", "1"}, 0},
            {{"data.collision.AggGeom.BoxElems.0.CollisionEnabled", R"("Sometimes")"}, 0},
            {{"data.collision.AggGeom.BoxElems.0.X", R"("wide")"}, 0},
            {{"data.collision.AggGeom.BoxElems.0.Name", "1"}, 0},
            {{"data.collision.AggGeom.BoxElems.0", "1"}, 0},
            {{"data.collision.AggGeom.ConvexElems",
              R"([{"VertexData": [{"X": 0, "Y": 0, "Z": 0}], "IndexData": [-1]}])"},
             0},
            {{"data.collision.AggGeom.ConvexElems", R"([{"VertexData": [1]}])"}, 0},
            {{"data.collision.AggGeom.ConvexElems", R"([{"Transform": 1}])"}, 0},
            {{"data.collision.AggGeom.ConvexElems",
              R"([{"Transform": {"Rotation": {"X": 0, "Y": 0, "Z": 0}}}])"},
             0},
            {{"data.collision.AggGeom.SphylElems", R"([{"Length": "x"}])"}, 0},
            {{"data.collision.AggGeom.SphereElems", R"([{}, {"Radius": "x"}])"}, 1},
            {{"data.collision.AggGeom.TaperedCapsuleElems", R"([{"Width": "x"}])"}, 0},
        };
        for (const auto& item : rejects) {
            CHECK(Rejects(env, allocator, cube, blobs, Span<const JsonEdit>(&item.edit, 1),
                          AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData, item.detail));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 정적 메시 표: 행 수, UE 키 중복 없음, 명세로 옮기는 행 번호가 enum과 같다.
TEST(UnrealExchange_StaticMeshSettingRows) {
    const Span<const ExchangeSettingRow> rows = ExchangeStaticMeshSettingRows();
    CHECK(rows.Size() == ExchangeStaticMeshSettingRowCount);
    for (usize index = 0; index < rows.Size(); ++index) {
        for (usize other = 0; other < index; ++other) {
            CHECK(rows[index].ueKey != rows[other].ueKey);
        }
    }

    CHECK(rows[static_cast<usize>(ExchangeStaticMeshSettingRow::SourceModels)].ueKey ==
              "SourceModels" &&
          rows[static_cast<usize>(ExchangeStaticMeshSettingRow::NaniteSettings)].ueKey ==
              "NaniteSettings" &&
          rows[static_cast<usize>(ExchangeStaticMeshSettingRow::ComplexCollisionMesh)].ueKey ==
              "ComplexCollisionMesh" &&
          rows[static_cast<usize>(ExchangeStaticMeshSettingRow::AutoComputeLodScreenSize)].ueKey ==
              "bAutoComputeLODScreenSize" &&
          rows[ExchangeStaticMeshSettingRowCount - 1].kind == ExchangeSettingKind::Unsupported);
}

} // namespace exchange_static_mesh_tests_file
