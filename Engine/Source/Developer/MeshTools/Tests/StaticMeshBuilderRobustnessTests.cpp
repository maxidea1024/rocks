#include "MeshToolsTestSupport.h"

#include <Mesh/MeshBuildSettings.generated.h>
#include <MeshSource/Tests/MeshSourceTestSupport.h>
#include <Object/TypeRegistry.h>
#include <Platform/FloatingPointEnvironment.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>

// 정적 메시 빌드의 결정성·설정·입력 오류·할당 실패·퍼징
// 시험이다(Docs/Design/MeshAnimationAssets 4.1절, 5.2절 결정성, 11절 진단, 12절 "빌드").
// 빌드는 스레드를 쓰지 않으므로(한 호출 = 한 스레드의 순수 함수) 12절의 "스레드 수 바꿔서" 결정성
// 시험은 대상이 없어 두지 않는다 — 스레드를 쓰게 되면 그때 더한다(5.4절).
using namespace rocks;
namespace static_mesh_builder_robustness_tests_file {

using namespace rocks::mesh_tools_test;

constexpr uint64 BuildFuzzSeed = 0x4d4132614255494cull;
constexpr uint32 BuildFuzzMeshCount = 400;

// 설정을 여러 가지로 바꿔 가며 쓰는 결정성 표본: 정육면체 + 구 + 공유·퇴화 메시.
bool BuildSampleMeshes(MeshSource& cube, MeshSource& sphere, MeshSource& shared) noexcept {
    return BuildCube(cube, 100.0f) && BuildUvSphere(sphere, 50.0f, 12, 6) &&
           mesh_source_test::BuildRichMesh(shared);
}

MeshBuildSettings SettingsVariant(uint32 variant) noexcept {
    MeshBuildSettings settings;
    settings.useMikkTSpace = (variant & 1) == 0;
    settings.computeWeightedNormals = (variant & 2) != 0;
    settings.removeDegenerates = (variant & 4) == 0;
    settings.recomputeNormals = (variant & 8) == 0;
    settings.buildScale = (variant & 16) != 0 ? Vec3d(1.5, -0.5, 2.0) : Vec3d(1.0, 1.0, 1.0);
    return settings;
}

// 같은 입력을 두 번 빌드하면 결과 byte가 같고, 호출 전 스레드 환경이 비정규 수를 0으로 버리게(FTZ)
// 바뀌어 있어도 같다(빌드가 RunWithDeterministicFloatingPoint 안에서 계산한다). 아주 작은 좌표의
// 메시가 비정규 중간값을 만든다.
TEST(StaticMeshBuild_DeterministicBytes) {
    SystemAllocator allocator;
    MeshSource cube(allocator);
    MeshSource sphere(allocator);
    MeshSource shared(allocator);
    REQUIRE(BuildSampleMeshes(cube, sphere, shared));
    MeshSource tiny(allocator);
    REQUIRE(BuildCube(tiny, 4.0e-19f));
    const MeshSource* meshes[] = {&cube, &sphere, &shared, &tiny};
    for (const MeshSource* mesh : meshes) {
        for (uint32 variant = 0; variant < 32; ++variant) {
            StaticMeshBuildInput input = InputFor(*mesh);
            input.settings = SettingsVariant(variant);
            StaticMeshBuildResult first(allocator);
            StaticMeshBuildResult second(allocator);
            StaticMeshBuildResult flushed(allocator);
            REQUIRE(BuildStaticMeshLod(input, first));
            REQUIRE(BuildStaticMeshLod(input, second));
            const uint64 original = platform::detail::ReadFloatingPointControl();
            platform::detail::WriteFloatingPointControl(original |
                                                        platform::detail::FlushToZeroBit());
            const bool built = BuildStaticMeshLod(input, flushed).IsOk();
            platform::detail::WriteFloatingPointControl(original);
            REQUIRE(built);
            // 임시 할당기를 따로 줘도 결과는 같다.
            SystemAllocator scratch;
            input.scratch = &scratch;
            StaticMeshBuildResult separate(allocator);
            REQUIRE(BuildStaticMeshLod(input, separate));
            const Array<uint8> a = CanonicalBytes(allocator, first);
            REQUIRE(!a.IsEmpty());
            CHECK(SameBytes(a, CanonicalBytes(allocator, second)));
            CHECK(SameBytes(a, CanonicalBytes(allocator, flushed)));
            CHECK(SameBytes(a, CanonicalBytes(allocator, separate)));
        }
    }
}

// 보존 설정(4.1절)은 기본값과 다르면 필드마다 UnsupportedSetting 경고 하나(index = 필드 번호, 필드
// 순서)이고 구현 설정은 경고가 없다. 실수 필드는 bit 비교라 -0.0도 기본값이 아니다.
TEST(StaticMeshBuild_PreservedSettingsWarn) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    StaticMeshBuildResult built(allocator);
    for (uint32 field = 0; field < MeshBuildSettingFieldCount; ++field) {
        StaticMeshBuildInput input = InputFor(mesh);
        SkinnedMeshBuildSettings changed;
        ChangeSetting(changed, static_cast<MeshBuildSettingField>(field));
        input.settings = changed;
        const MeshBuildSettings& settings = input.settings;
        const auto value = static_cast<MeshBuildSettingField>(field);
        CHECK(IsMeshBuildSettingChanged(settings, value));
        REQUIRE(BuildStaticMeshLod(input, built));
        const uint32 expected = IsPreservedMeshBuildSetting(value) ? 1 : 0;
        CHECK(CountWarnings(built, AssetDiagnosticCode::UnsupportedSetting) == expected);
        for (const MeshDiagnostic& warning : built.warnings) {
            if (warning.code == AssetDiagnosticCode::UnsupportedSetting) {
                CHECK(warning.index == field && warning.issue == MeshIssue::UnsupportedSetting);
            }
        }
    }

    // 모두 바꾸면 보존 필드 9개가 필드 순서로 나온다.
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.buildReversedIndexBuffer = false;
    input.settings.generateLightmapUVs = false;
    input.settings.minLightmapResolution = 32;
    input.settings.srcLightmapIndex = 2;
    input.settings.dstLightmapIndex = 3;
    input.settings.generateDistanceFieldAsIfTwoSided = true;
    input.settings.distanceFieldResolutionScale = 2.0f;
    input.settings.maxLumenMeshCards = 0;
    input.settings.supportFaceRemap = true;
    REQUIRE(BuildStaticMeshLod(input, built));
    uint32 previous = 0;
    uint32 count = 0;
    for (const MeshDiagnostic& warning : built.warnings) {
        if (warning.code == AssetDiagnosticCode::UnsupportedSetting) {
            CHECK(count == 0 || warning.index > previous);
            previous = warning.index;
            ++count;
        }
    }

    CHECK(count == 9);
}

// 설정 필드 표: 이름마다 같은 이름의 리플렉션 필드가 하나 있고(필드 수도 같다), 기본값은 UE
// FMeshBuildSettings 생성자와 같으며, 모르는 번호는 "Unknown"·보존 아님·바뀌지 않음이다.
TEST(StaticMeshBuild_SettingFieldTable) {
    const TypeSchema& schema = GeneratedType<MeshBuildSettings>::Schema();
    REQUIRE(schema.fields.Size() == MeshBuildSettingFieldCount);
    uint32 preserved = 0;
    for (uint32 field = 0; field < MeshBuildSettingFieldCount; ++field) {
        const auto value = static_cast<MeshBuildSettingField>(field);
        const StringView name(MeshBuildSettingFieldName(value));
        CHECK(name != StringView("Unknown"));
        uint32 matches = 0;
        for (const FieldSchema& reflected : schema.fields) {
            matches += name == StringView(reflected.name.Data(), reflected.name.Size()) ? 1 : 0;
        }

        CHECK(matches == 1);
        CHECK(!IsMeshBuildSettingChanged(MeshBuildSettings{}, value));
        preserved += IsPreservedMeshBuildSetting(value) ? 1 : 0;
    }

    CHECK(preserved == 9);
    const auto outside = static_cast<MeshBuildSettingField>(SkinnedMeshBuildSettingFieldCount);
    CHECK(StringView(MeshBuildSettingFieldName(outside)) == StringView("Unknown"));
    CHECK(!IsPreservedMeshBuildSetting(outside));
    // 스킨 필드는 정적 판정에서 보존이 아니고 바뀐 것으로 보지 않는다.
    for (uint32 field = MeshBuildSettingFieldCount; field < SkinnedMeshBuildSettingFieldCount;
         ++field) {
        const auto skinned = static_cast<MeshBuildSettingField>(field);
        CHECK(!IsPreservedMeshBuildSetting(skinned) &&
              !IsMeshBuildSettingChanged(MeshBuildSettings{}, skinned));
    }

    MeshBuildSettings changed;
    changed.buildScale = Vec3d(1.0, 1.0, -0.0);
    CHECK(!IsMeshBuildSettingChanged(changed, outside));
    CHECK(IsMeshBuildSettingChanged(changed, MeshBuildSettingField::BuildScale));
    changed.buildScale = Vec3d(1.0, 2.0, 1.0);
    CHECK(IsMeshBuildSettingChanged(changed, MeshBuildSettingField::BuildScale));
    // 모듈 타입 등록(도구·시험이 직접 부른다).
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    CHECK(RegisterMeshTypes(modules));
    CHECK(modules.Unregister());
    const MeshBuildSettings defaults;
    CHECK(defaults.recomputeNormals && defaults.recomputeTangents && defaults.useMikkTSpace);
    CHECK(!defaults.computeWeightedNormals && defaults.removeDegenerates);
    CHECK(defaults.buildReversedIndexBuffer && defaults.generateLightmapUVs);
    CHECK(!defaults.useHighPrecisionTangentBasis && !defaults.useFullPrecisionUVs);
    CHECK(defaults.minLightmapResolution == 64 && defaults.srcLightmapIndex == 0 &&
          defaults.dstLightmapIndex == 1 && defaults.maxLumenMeshCards == 12);
    CHECK(defaults.buildScale == Vec3d(1.0, 1.0, 1.0) &&
          defaults.distanceFieldResolutionScale == 1.0f);
    CHECK(!defaults.generateDistanceFieldAsIfTwoSided && !defaults.supportFaceRemap);
}

// 입력 오류: source null(진단 없음), 검증 실패(원본 진단 그대로), buildScale 성분 0·NaN·무한·float
// 범위 밖·역수 무한, 크기를 곱한 위치 넘침(InvalidSetting, issue InvalidBuildScale, index = 축).
// 실패하면 결과는 그대로다.
TEST(StaticMeshBuild_RejectsInvalidInputs) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
    const Array<uint8> before = CanonicalBytes(allocator, built);

    StaticMeshBuildInput none;
    MeshDiagnostic failure;
    auto result = BuildStaticMeshLod(none, built, &failure);
    CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::None);

    MeshSource broken(allocator);
    REQUIRE(mesh_source_test::BuildQuad(broken));
    REQUIRE(broken.AddCorner(99));
    result = BuildStaticMeshLod(InputFor(broken), built, &failure);
    CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::InvalidMeshData);
    CHECK(failure.issue == MeshIssue::CornerVertexRange && failure.index == 4);

    struct Case {
        Vec3d scale;
        uint32 axis;
    };
    const double nan = static_cast<double>(mesh_source_test::TestQuietNaN());
    const double infinity = static_cast<double>(mesh_source_test::TestInfinity());
    const Case cases[] = {{Vec3d(0.0, 1.0, 1.0), 0},      {Vec3d(1.0, nan, 1.0), 1},
                          {Vec3d(1.0, 1.0, infinity), 2}, {Vec3d(1.0, 1.0e39, 1.0), 1},
                          {Vec3d(1.0, 1.0, 1.0e-46), 2},  {Vec3d(-0.0, 1.0, 1.0), 0},
                          {Vec3d(1.0, 1.0, 1.0e-39), 2}};
    for (const Case& item : cases) {
        StaticMeshBuildInput input = InputFor(mesh);
        input.settings.buildScale = item.scale;
        MeshDiagnostic scaled;
        result = BuildStaticMeshLod(input, built, &scaled);
        CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
        CHECK(scaled.code == AssetDiagnosticCode::InvalidSetting);
        CHECK(scaled.issue == MeshIssue::InvalidBuildScale && scaled.index == item.axis);
        CHECK(scaled.error == ErrorCode::InvalidArgument);
    }

    // 큰 크기로 위치가 float 범위를 넘는다(축 1).
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.buildScale = Vec3d(1.0, 1.0e37, 1.0);
    MeshDiagnostic overflow;
    result = BuildStaticMeshLod(input, built, &overflow);
    CHECK(!result && overflow.code == AssetDiagnosticCode::InvalidSetting && overflow.index == 1);
    CHECK(SameBytes(before, CanonicalBytes(allocator, built)));
}

// 할당 실패 주입: n번째 할당을 실패시키면 빌드는 그 오류(OutOfMemory)로 끝나고 결과는 그대로이거나,
// MikkTSpace가 실패를 느린 경로로 넘긴 경우 성공하며 그 결과는 실패 없는 빌드와 같은 byte다. 모든
// 할당 지점을 차례로 지난다(결과·임시 할당기 모두 같은 실패 할당기).
TEST(StaticMeshBuild_AllocationFailures) {
    SystemAllocator base;
    MeshSource mesh(base);
    REQUIRE(BuildCube(mesh, 100.0f));
    MeshSource clean(base);
    REQUIRE(BuildCube(clean, 100.0f));
    const uint32 color = CornerAttribute(mesh, MeshAttributeName::Color, MeshValueType::Vec4);
    mesh.SetVec4(color, 0, 0, Vec4(mesh_source_test::TestQuietNaN(), 0.0f, 0.0f, 1.0f));
    // 변형 0–3: 값 정리 경고 + 보존 설정 경고 + 음수 크기(설정 넷). 4: 경고는 슬롯 대체뿐(그 경고가
    // 경고 배열을 처음 할당한다). 5: 보존 설정 경고가 처음 할당한다.
    for (uint32 variant = 0; variant < 6; ++variant) {
        StaticMeshBuildInput input = InputFor(variant < 4 ? mesh : clean);
        input.settings = variant < 4 ? SettingsVariant(variant | 16) : MeshBuildSettings{};
        input.settings.generateLightmapUVs = variant == 4;
        StaticMeshBuildResult expected(base);
        REQUIRE(BuildStaticMeshLod(input, expected));
        const Array<uint8> golden = CanonicalBytes(base, expected);
        uint32 failures = 0;
        uint32 recovered = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(base);
            StaticMeshBuildResult built(failing);
            failing.FailOnAttempt(attempt);
            auto result = BuildStaticMeshLod(input, built);
            const bool reached = failing.Attempts() >= attempt;
            if (!result) {
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                CHECK(built.VertexCount() == 0 && built.warnings.IsEmpty());
                ++failures;
                continue;
            }

            CHECK(SameBytes(golden, CanonicalBytes(base, built)));
            if (!reached) {
                break;
            }

            ++recovered;
        }

        CHECK(failures > 100);
        printf("StaticMeshBuild_AllocationFailures variant %u: %u failures, %u recovered\n",
               variant, failures, recovered);
    }
}

// 아주 큰 값: 좌표 1e20(외적·면적이 float를 넘친다)·UV 1e20(UV 행렬식이 넘친다)·원본 법선과
// tangent 1e30(정규화가 NaN을 만든다)에서도 빌드는 성공하고 법선·tangent는 유한(넘친 값은 0
// 벡터)이며 두 번 빌드한 byte가 같다(호스트마다 다른 NaN bit가 결과에 남지 않는다). 아주 작은
// 삼각형(변 1e-5, removeDegenerates 끔)은 UV tangent가 정규화 하한 아래라 0 벡터가 된다. tangent가
// 법선과 평행한 원본 코너는 유효하지 않아 다시 계산한다.
TEST(StaticMeshBuild_ExtremeValuesStayFinite) {
    SystemAllocator allocator;
    const uint32 triangle[] = {0, 1, 2};
    const PolygonSpec polygons[] = {{triangle, 0}};
    const Vec3 huge[] = {Vec3(0, 0, 0), Vec3(1.0e20f, 0, 0), Vec3(0, 1.0e20f, 1.0e19f)};
    const Vec3 tiny[] = {Vec3(0, 0, 0), Vec3(1.0e-5f, 0, 0), Vec3(0, 1.0e-5f, 0)};
    for (uint32 variant = 0; variant < 4; ++variant) {
        MeshSource mesh(allocator);
        REQUIRE(BuildPolygonMesh(mesh, variant == 3 ? tiny : huge, polygons, 1, true));
        // 변형 1은 UV 1e20(행렬식 넘침), 변형 3은 UV 0–1(아주 작은 삼각형의 tangent 축이 하한
        // 아래).
        const float uvScale = variant == 1 ? 1.0e-20f : (variant == 3 ? 1.0e-5f : 1.0f);
        const uint32 uv = PlanarUvs(mesh, uvScale);
        REQUIRE(uv != MeshNoAttribute);
        StaticMeshBuildInput input = InputFor(mesh);
        // 아주 작은 삼각형은 가중치(면적 × 각)가 UE 하한(1e-8) 아래라 법선이 빠지므로 가중을 끈다.
        input.settings.computeWeightedNormals = variant != 3;
        input.settings.removeDegenerates = variant != 3;
        if (variant == 2) {
            const uint32 normal =
                CornerAttribute(mesh, MeshAttributeName::Normal, MeshValueType::Vec3);
            const uint32 tangent =
                CornerAttribute(mesh, MeshAttributeName::Tangent, MeshValueType::Vec3);
            mesh.SetVec3(normal, 0, 0, Vec3(1.0e30f, 1.0e30f, 1.0e30f));
            mesh.SetVec3(tangent, 0, 0, Vec3(1.0e30f, 0.0f, 0.0f));
            mesh.SetVec3(normal, 0, 1, Vec3(0.0f, 0.0f, 1.0f));
            mesh.SetVec3(tangent, 0, 1, Vec3(0.0f, 0.0f, 1.0f));
            mesh.SetVec3(normal, 0, 2, Vec3(0.0f, 1.0e30f, 1.0e30f));
            mesh.SetVec3(tangent, 0, 2, Vec3(1.0f, 0.0f, 0.0f));
            input.settings.recomputeNormals = false;
            input.settings.recomputeTangents = false;
        }

        StaticMeshBuildResult built(allocator);
        StaticMeshBuildResult again(allocator);
        REQUIRE(BuildStaticMeshLod(input, built));
        REQUIRE(BuildStaticMeshLod(input, again));
        REQUIRE(built.VertexCount() == 3);
        for (uint32 vertex = 0; vertex < 3; ++vertex) {
            CHECK(FiniteVector(built.normals[vertex]) && FiniteVector(built.tangents[vertex]));
        }

        CHECK(math::IsFinite(built.bounds.sphereRadius));
        CHECK(SameBytes(CanonicalBytes(allocator, built), CanonicalBytes(allocator, again)));
        if (variant == 3) {
            CHECK(Near(built.normals[0].Length(), 1.0f, 1.0e-5f));
        }
    }
}

// 고정 seed 퍼징: MA1 퍼저가 만든 메시를 무작위 설정으로 빌드한다. 검증을 통과하지 못한 메시는 같은
// 검증 진단으로 실패하고, 통과한 메시는 성공하며 결과가 범위 안(index < 정점 수, 섹션 범위, 유한한
// 값·bounds)이고 두 번 빌드한 byte가 같다. 범위 밖 읽기는 단언으로 멈추므로 끝까지 오면 그것도
// 지켰다.
TEST(StaticMeshBuild_FuzzSourceMeshes) {
    SystemAllocator allocator;
    RandomStream random(BuildFuzzSeed);
    uint32 accepted = 0;
    uint32 rejected = 0;
    uint32 renderTriangles = 0;
    for (uint32 iteration = 0; iteration < BuildFuzzMeshCount; ++iteration) {
        MeshSource mesh(allocator);
        REQUIRE(mesh_source_test::RandomFuzzMesh(random, mesh));
        REQUIRE(mesh_source_test::RandomFuzzAttributes(random, mesh));
        StaticMeshBuildInput input = InputFor(mesh);
        input.settings = SettingsVariant(random.NextUInt32Below(32));
        const StringView slots[] = {"Body", "Visor_2"};
        input.slotNames = Span<const StringView>(slots, random.NextUInt32Below(3));
        MeshDiagnostic validation;
        const bool valid = ValidateMeshSource(mesh, {}, &validation).IsOk();
        StaticMeshBuildResult built(allocator);
        MeshDiagnostic failure;
        auto result = BuildStaticMeshLod(input, built, &failure);
        if (!valid) {
            ++rejected;
            CHECK(!result && failure.code == validation.code && failure.issue == validation.issue);
            continue;
        }

        REQUIRE(result);
        ++accepted;
        const uint32 vertices = built.VertexCount();
        CHECK(built.normals.Size() == vertices && built.tangents.Size() == vertices);
        CHECK(built.binormalSigns.Size() == vertices);
        CHECK(built.texCoords.Size() == static_cast<usize>(vertices) * built.texCoordChannels);
        CHECK(built.indices.Size() % 3 == 0);
        CHECK(built.sections.Size() == mesh.Count(MeshElement::PolygonGroup));
        for (uint32 index : built.indices) {
            CHECK(index < vertices);
        }

        uint32 covered = 0;
        for (const MeshBuildSection& section : built.sections) {
            covered += section.triangleCount * 3;
            CHECK(section.firstIndex + section.triangleCount * 3 <= built.indices.Size());
            CHECK(section.minVertex <= section.maxVertex);
        }

        CHECK(covered == built.indices.Size());
        for (uint32 vertex = 0; vertex < vertices; ++vertex) {
            CHECK(FiniteVector(built.normals[vertex]) && FiniteVector(built.tangents[vertex]));
            CHECK(built.binormalSigns[vertex] == 1.0f || built.binormalSigns[vertex] == -1.0f);
        }

        CHECK(math::IsFinite(built.bounds.sphereRadius));
        renderTriangles += static_cast<uint32>(built.indices.Size() / 3);
        StaticMeshBuildResult again(allocator);
        REQUIRE(BuildStaticMeshLod(input, again));
        CHECK(SameBytes(CanonicalBytes(allocator, built), CanonicalBytes(allocator, again)));
    }

    CHECK(accepted >= BuildFuzzMeshCount / 8);
    CHECK(rejected >= BuildFuzzMeshCount / 8);
    CHECK(renderTriangles != 0);
}

} // namespace static_mesh_builder_robustness_tests_file
