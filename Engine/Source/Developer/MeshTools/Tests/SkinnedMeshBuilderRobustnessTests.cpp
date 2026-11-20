#include "SkinnedMeshTestSupport.h"

#include <Mesh/MeshBuildSettings.generated.h>
#include <MeshSource/Tests/MeshSourceTestSupport.h>
#include <Platform/FloatingPointEnvironment.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>

// 스킨 메시 빌드의 설정·입력 오류·정적 빌드와의 일치·결정성·할당 실패·퍼징
// 시험이다(Docs/Design/MeshAnimationAssets.ko.md 6.5절, 11절 진단, 12절 "스켈레톤·스킨").
using namespace rocks;
namespace skinned_mesh_builder_robustness_tests_file {

using namespace rocks::skinned_mesh_test;

constexpr uint64 SkinFuzzSeed = 0x4d413561534b494eull;
constexpr uint32 SkinFuzzMeshCount = 400;
// 퍼징 메시의 boneIndex(많아야 33)를 담는 본 수다.
constexpr uint32 SkinFuzzBones = 40;

// 가중치·프로필·morph(재계산·속성)·버린 영향·빠진 영향을 모두 가진 표본이다.
bool BuildRichSkin(MeshSource& mesh) noexcept {
    if (!BuildGrid(mesh, 4, 50.0f) || PlanarUvs(mesh, 150.0f) == MeshNoAttribute) {
        return false;
    }

    const uint32 weights = AddWeights(mesh, MeshAttributeName::SkinWeights);
    const uint32 profile = AddWeights(mesh, "SkinWeights-Cloth");
    const uint32 raise = AddSparse(mesh, MeshElement::Vertex, "Morph-Raise");
    const uint32 smile = AddSparse(mesh, MeshElement::Vertex, "Morph-Smile");
    const uint32 smileNormal = AddSparse(mesh, MeshElement::Corner, "MorphNormal-Smile");
    if (weights == MeshNoAttribute || profile == MeshNoAttribute || raise == MeshNoAttribute ||
        smile == MeshNoAttribute || smileNormal == MeshNoAttribute) {
        return false;
    }

    for (uint32 vertex = 1; vertex < 16; ++vertex) {
        BoneInfluence influences[10] = {};
        const uint32 used = 1 + vertex % 10;
        uint32 total = 0;
        for (uint32 at = 0; at < used; ++at) {
            influences[at].boneIndex = static_cast<uint16>((vertex + at) % 12);
            influences[at].weight = static_cast<uint16>(at + 1 == used ? 0 : 3000 + 977 * at);
            total += influences[at].weight;
        }

        influences[used - 1].weight = static_cast<uint16>(BoneWeightTotal - total);
        const BoneInfluence cloth{static_cast<uint16>(vertex % 5), BoneWeightTotal};
        if (!SetWeights(mesh, weights, vertex, Span<const BoneInfluence>(influences, used)) ||
            (vertex % 3 != 0 &&
             !SetWeights(mesh, profile, vertex, Span<const BoneInfluence>(&cloth, 1)))) {
            return false;
        }
    }

    return mesh.SetSparseValue(raise, 5, Vec3(0, 0, 20)) &&
           mesh.SetSparseValue(raise, 10, Vec3(3, 0, 1)) &&
           mesh.SetSparseValue(smile, 6, Vec3(0.5f, 0, 0)) &&
           mesh.SetSparseValue(smileNormal, 9, Vec3(0, 0.3f, 0));
}

// 설정 필드 표: SkinnedMeshBuildSettings 리플렉션 필드(상속 펼침 포함)와 이름이 하나씩 맞고, 여섯
// 스킨 필드의 기본값은 UE와 같다. 스킨 빌드의 보존 필드는 정적 9개 + buildScale이다.
TEST(SkinnedMeshBuild_SettingFieldTable) {
    const TypeSchema& schema = GeneratedType<SkinnedMeshBuildSettings>::Schema();
    REQUIRE(schema.fields.Size() == SkinnedMeshBuildSettingFieldCount);
    CHECK(schema.baseType == GeneratedType<MeshBuildSettings>::Id());
    uint32 preserved = 0;
    for (uint32 field = 0; field < SkinnedMeshBuildSettingFieldCount; ++field) {
        const auto value = static_cast<MeshBuildSettingField>(field);
        const StringView name(MeshBuildSettingFieldName(value));
        CHECK(name != StringView("Unknown"));
        uint32 matches = 0;
        for (const FieldSchema& reflected : schema.fields) {
            matches += name == StringView(reflected.name.Data(), reflected.name.Size()) ? 1 : 0;
        }

        CHECK(matches == 1);
        SkinnedMeshBuildSettings changed;
        CHECK(!IsMeshBuildSettingChanged(changed, value));
        ChangeSetting(changed, value);
        CHECK(IsMeshBuildSettingChanged(changed, value));
        // 정적 판정은 스킨 필드를 보지 않는다.
        CHECK(IsMeshBuildSettingChanged(static_cast<const MeshBuildSettings&>(changed), value) ==
              (field < MeshBuildSettingFieldCount));
        preserved += IsPreservedSkinnedMeshBuildSetting(value) ? 1 : 0;
        CHECK(IsPreservedSkinnedMeshBuildSetting(value) ==
              (IsPreservedMeshBuildSetting(value) || value == MeshBuildSettingField::BuildScale));
    }

    CHECK(preserved == 10);
    const auto outside = static_cast<MeshBuildSettingField>(SkinnedMeshBuildSettingFieldCount);
    CHECK(StringView(MeshBuildSettingFieldName(outside)) == StringView("Unknown"));
    CHECK(!IsPreservedSkinnedMeshBuildSetting(outside));
    CHECK(!IsMeshBuildSettingChanged(SkinnedMeshBuildSettings{}, outside));
    SkinnedMeshBuildSettings negativeZero;
    negativeZero.thresholdPosition = -0.0f;
    CHECK(IsMeshBuildSettingChanged(negativeZero, MeshBuildSettingField::ThresholdPosition));
    const SkinnedMeshBuildSettings defaults;
    CHECK(!defaults.useHighPrecisionSkinWeights && defaults.boneInfluenceLimit == 0);
    CHECK(defaults.thresholdPosition == 0.00002f && defaults.thresholdTangentNormal == 0.00002f);
    CHECK(defaults.thresholdUV == 0.0009765625f && defaults.morphThresholdPosition == 0.015f);
    CHECK(defaults.thresholdPosition == MeshPointsSameThreshold &&
          defaults.thresholdUV == MeshUvsSameThreshold);
}

// 보존 필드(buildScale 포함)마다 UnsupportedSetting 경고 하나(index = 필드), 구현 필드는 경고가
// 없다. buildScale은 적용하지 않고 NegativeBuildScale도 내지 않는다.
TEST(SkinnedMeshBuild_PreservedSettingsWarn) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 2, bones));
    MeshSource mesh(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    REQUIRE(RigidWeights(mesh, 1) != MeshNoAttribute);
    for (uint32 field = 0; field < SkinnedMeshBuildSettingFieldCount; ++field) {
        const auto value = static_cast<MeshBuildSettingField>(field);
        SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
        ChangeSetting(input.settings, value);
        if (value == MeshBuildSettingField::BuildScale) {
            input.settings.buildScale = Vec3d(2.0, -1.0, 1.0);
        }

        SkinnedMeshBuildResult built(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, built));
        uint32 warnings = 0;
        for (const MeshDiagnostic& warning : built.mesh.warnings) {
            CHECK(warning.code != AssetDiagnosticCode::NegativeBuildScale);
            if (warning.code == AssetDiagnosticCode::UnsupportedSetting) {
                CHECK(warning.index == field && warning.issue == MeshIssue::UnsupportedSetting);
                ++warnings;
            }
        }

        CHECK(warnings == (IsPreservedSkinnedMeshBuildSetting(value) ? 1u : 0u));
        CHECK(built.mesh.bounds.boxMax == Vec3d(100.0, 100.0, 0.0));
    }
}

// 입력 오류: source·bones null(진단 없음), 임계값 NaN·무한·음수, boneInfluenceLimit 음수
// (InvalidSetting·InvalidSettingValue, index = 필드), 본 없음·계층 위반·역이 없는 기준 pose
// (InvalidSkeletonData), boneIndex 범위 밖·가중치 합 0(원본 진단, 속성은 원본 번호). 실패하면
// 결과는 그대로다.
TEST(SkinnedMeshBuild_RejectsInvalidInputs) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 3, bones));
    MeshSource mesh(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    REQUIRE(mesh.AddAttribute(MeshElement::Vertex, "Heat", MeshValueType::Float, 1));
    REQUIRE(RigidWeights(mesh, 2) == 1);
    SkinnedMeshBuildResult kept(fixture.allocator);
    REQUIRE(BuildSkinnedMeshLod(SkinInputFor(mesh, bones), kept));
    const Array<uint8> golden = SkinBytes(fixture.allocator, kept);
    REQUIRE(!golden.IsEmpty());
    auto expectFailure = [&](const SkinnedMeshBuildInput& input, AssetDiagnosticCode code,
                             MeshIssue issue, uint32 index, ErrorCode error) noexcept {
        MeshDiagnostic failure;
        auto result = BuildSkinnedMeshLod(input, kept, &failure);
        CHECK(!result && result.Error() == error);
        CHECK(failure.code == code && failure.issue == issue && failure.index == index &&
              failure.error == error);
        CHECK(SameBytes(golden, SkinBytes(fixture.allocator, kept)));
    };

    SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
    input.source = nullptr;
    MeshDiagnostic untouched;
    CHECK(BuildSkinnedMeshLod(input, kept, &untouched).Error() == ErrorCode::InvalidArgument);
    CHECK(untouched.code == AssetDiagnosticCode::None);
    input = SkinInputFor(mesh, bones);
    input.bones = nullptr;
    CHECK(BuildSkinnedMeshLod(input, kept).Error() == ErrorCode::InvalidArgument);
    const float bad[] = {mesh_source_test::TestQuietNaN(), mesh_source_test::TestInfinity(),
                         -0.001f};
    const MeshBuildSettingField thresholds[] = {
        MeshBuildSettingField::ThresholdPosition, MeshBuildSettingField::ThresholdTangentNormal,
        MeshBuildSettingField::ThresholdUV, MeshBuildSettingField::MorphThresholdPosition};
    for (MeshBuildSettingField field : thresholds) {
        for (float value : bad) {
            input = SkinInputFor(mesh, bones);
            float* target = field == MeshBuildSettingField::ThresholdPosition
                                ? &input.settings.thresholdPosition
                            : field == MeshBuildSettingField::ThresholdTangentNormal
                                ? &input.settings.thresholdTangentNormal
                            : field == MeshBuildSettingField::ThresholdUV
                                ? &input.settings.thresholdUV
                                : &input.settings.morphThresholdPosition;
            *target = value;
            expectFailure(input, AssetDiagnosticCode::InvalidSetting,
                          MeshIssue::InvalidSettingValue, static_cast<uint32>(field),
                          ErrorCode::InvalidArgument);
        }
    }

    input = SkinInputFor(mesh, bones);
    input.settings.boneInfluenceLimit = -1;
    expectFailure(input, AssetDiagnosticCode::InvalidSetting, MeshIssue::InvalidSettingValue,
                  static_cast<uint32>(MeshBuildSettingField::BoneInfluenceLimit),
                  ErrorCode::InvalidArgument);
    // 임계값 0은 받는다(정확히 같은 값만).
    input = SkinInputFor(mesh, bones);
    input.settings.thresholdPosition = -0.0f;
    input.settings.morphThresholdPosition = 0.0f;
    SkinnedMeshBuildResult zero(fixture.allocator);
    CHECK(BuildSkinnedMeshLod(input, zero));
    // 본 계층.
    BoneHierarchy empty(fixture.allocator);
    input = SkinInputFor(mesh, empty);
    expectFailure(input, AssetDiagnosticCode::InvalidSkeletonData, MeshIssue::EmptyBoneHierarchy,
                  MeshNoIndex, ErrorCode::InvalidArgument);
    BoneHierarchy broken(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 3, broken));
    broken.bones[2].parentIndex = 2;
    input = SkinInputFor(mesh, broken);
    expectFailure(input, AssetDiagnosticCode::InvalidSkeletonData, MeshIssue::InvalidBoneHierarchy,
                  2, ErrorCode::InvalidArgument);
    BoneHierarchy flat(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 3, flat));
    flat.referencePose[1].scale = Vec3d(1.0, 0.0, 1.0);
    input = SkinInputFor(mesh, flat);
    expectFailure(input, AssetDiagnosticCode::InvalidSkeletonData, MeshIssue::SingularBindPose, 1,
                  ErrorCode::InvalidArgument);
    // 원본: 본 수(2) 밖의 boneIndex, 가중치 합 0.
    BoneHierarchy two(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 2, two));
    input = SkinInputFor(mesh, two);
    MeshDiagnostic range;
    CHECK(!BuildSkinnedMeshLod(input, kept, &range));
    CHECK(range.code == AssetDiagnosticCode::InvalidMeshData &&
          range.issue == MeshIssue::BoneIndexRange && range.attribute == 1 && range.index == 0);
    MeshSource zeroSum(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(zeroSum));
    REQUIRE(zeroSum.AddAttribute(MeshElement::Vertex, "Heat", MeshValueType::Float, 1));
    const uint32 weights = AddWeights(zeroSum, MeshAttributeName::SkinWeights);
    const BoneInfluence nothing{1, 0};
    REQUIRE(SetWeights(zeroSum, weights, 2, Span<const BoneInfluence>(&nothing, 1)));
    MeshDiagnostic sum;
    CHECK(!BuildSkinnedMeshLod(SkinInputFor(zeroSum, bones), kept, &sum));
    CHECK(sum.code == AssetDiagnosticCode::InvalidMeshData &&
          sum.issue == MeshIssue::ZeroWeightSum && sum.attribute == weights && sum.index == 2);
    CHECK(SameBytes(golden, SkinBytes(fixture.allocator, kept)));
    // 정규화하지 않은 가중치는 경고(원본 속성 번호 — 프로필도)와 함께 정규화된다.
    const BoneInfluence half[] = {{0, 100}, {1, 300}};
    REQUIRE(SetWeights(zeroSum, weights, 2, half));
    const uint32 cloth = AddWeights(zeroSum, "SkinWeights-Cloth");
    REQUIRE(SetWeights(zeroSum, cloth, 1, half));
    SkinnedMeshBuildResult normalized(fixture.allocator);
    REQUIRE(BuildSkinnedMeshLod(SkinInputFor(zeroSum, bones), normalized));
    const MeshDiagnostic* warning =
        FindWarning(normalized, AssetDiagnosticCode::NonNormalizedWeights);
    REQUIRE(warning != nullptr);
    CHECK(warning->attribute == weights && warning->count == 1);
    const MeshDiagnostic* clothWarning =
        FindWarning(normalized, AssetDiagnosticCode::NonNormalizedWeights, cloth);
    REQUIRE(clothWarning != nullptr);
    CHECK(clothWarning->count == 1 && warning < clothWarning);
    CHECK(InfluencesWellFormed(normalized, VertexInfluences(normalized, normalized.influences, 2)));
}

// 스킨이 렌더 정점을 더 가르지 않는 메시(정점마다 같은 영향, morph 없음)는 스킨 임계값이 정적
// 상수와 같을 때 정적 빌드와 렌더 부분이 같다(5.2절 단계 공유, 6.5절 15).
TEST(SkinnedMeshBuild_RenderPartMatchesStaticBuild) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 2, bones));
    MeshSource cube(fixture.allocator);
    MeshSource sphere(fixture.allocator);
    MeshSource shared(fixture.allocator);
    REQUIRE(BuildCube(cube, 100.0f) && BuildUvSphere(sphere, 50.0f, 12, 6) &&
            mesh_source_test::BuildSharedMesh(shared));
    MeshSource* meshes[] = {&cube, &sphere, &shared};
    for (MeshSource* mesh : meshes) {
        REQUIRE(RigidWeights(*mesh, 1) != MeshNoAttribute);
        for (uint32 variant = 0; variant < 8; ++variant) {
            SkinnedMeshBuildInput skinned = SkinInputFor(*mesh, bones);
            skinned.settings.useMikkTSpace = (variant & 1) == 0;
            skinned.settings.computeWeightedNormals = (variant & 2) != 0;
            skinned.settings.recomputeNormals = (variant & 4) == 0;
            StaticMeshBuildInput plain = InputFor(*mesh);
            plain.settings = skinned.settings;
            SkinnedMeshBuildResult built(fixture.allocator);
            StaticMeshBuildResult expected(fixture.allocator);
            REQUIRE(BuildSkinnedMeshLod(skinned, built));
            REQUIRE(BuildStaticMeshLod(plain, expected));
            built.mesh.warnings.Clear();
            expected.warnings.Clear();
            CHECK(SameBytes(CanonicalBytes(fixture.allocator, expected),
                            CanonicalBytes(fixture.allocator, built.mesh)));
            CHECK(built.boneMap.Size() == 1 && built.boneMap[0] == 1);
        }
    }
}

// 같은 입력을 두 번, 호출 전 FTZ를 켜고, 임시 할당기를 따로 줘도 결과 byte가 같다.
TEST(SkinnedMeshBuild_DeterministicBytes) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 12, bones));
    MeshSource mesh(fixture.allocator);
    REQUIRE(BuildRichSkin(mesh));
    for (uint32 variant = 0; variant < 16; ++variant) {
        SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
        input.settings.useHighPrecisionSkinWeights = (variant & 1) != 0;
        input.settings.boneInfluenceLimit = (variant & 2) != 0 ? 4 : 0;
        input.settings.computeWeightedNormals = (variant & 4) != 0;
        input.settings.recomputeNormals = (variant & 8) == 0;
        SkinnedMeshBuildResult first(fixture.allocator);
        SkinnedMeshBuildResult second(fixture.allocator);
        SkinnedMeshBuildResult flushed(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, first));
        REQUIRE(BuildSkinnedMeshLod(input, second));
        const uint64 original = platform::detail::ReadFloatingPointControl();
        platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit());
        const bool built = BuildSkinnedMeshLod(input, flushed).IsOk();
        platform::detail::WriteFloatingPointControl(original);
        REQUIRE(built);
        SystemAllocator scratch;
        input.scratch = &scratch;
        SkinnedMeshBuildResult separate(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, separate));
        const Array<uint8> a = SkinBytes(fixture.allocator, first);
        REQUIRE(!a.IsEmpty());
        CHECK(SameBytes(a, SkinBytes(fixture.allocator, second)));
        CHECK(SameBytes(a, SkinBytes(fixture.allocator, flushed)));
        CHECK(SameBytes(a, SkinBytes(fixture.allocator, separate)));
        CHECK(first.profiles.Size() == 1 && first.morphTargets.Size() == 2);
        CHECK(!first.morphTargets[0].deltas.IsEmpty() && !first.morphTargets[1].deltas.IsEmpty());
    }
}

// 할당 실패 주입: 몇 번째 할당이 실패해도 OutOfMemory이고 결과는 그대로이며, 실패 없이 끝난 빌드는
// 기준 byte와 같다. 경고를 처음 할당하는 단계가 다른 변형(값 정리·설정·영향·morph)을 돈다.
TEST(SkinnedMeshBuild_AllocationFailures) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 12, bones));
    MeshSource rich(fixture.allocator);
    REQUIRE(BuildRichSkin(rich));
    MeshSource quad(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(quad));
    MeshSource normalized(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(normalized));
    const uint32 weights = AddWeights(normalized, MeshAttributeName::SkinWeights);
    const BoneInfluence half[] = {{0, 100}, {1, 300}};
    REQUIRE(SetWeights(normalized, weights, 0, half));
    MeshSource morphOnly(fixture.allocator);
    REQUIRE(BuildGrid(morphOnly, 3, 10.0f) && RigidWeights(morphOnly, 0) != MeshNoAttribute);
    MeshSource normalOnly(fixture.allocator);
    REQUIRE(BuildGrid(normalOnly, 3, 10.0f) && RigidWeights(normalOnly, 0) != MeshNoAttribute);
    const uint32 nan = AddSparse(morphOnly, MeshElement::Vertex, "Morph-Bad");
    REQUIRE(morphOnly.SetSparseValue(nan, 1, Vec3(mesh_source_test::TestQuietNaN(), 0, 0)));
    const uint32 nanNormal = AddSparse(normalOnly, MeshElement::Corner, "MorphNormal-Bad");
    REQUIRE(normalOnly.SetSparseValue(nanNormal, 1, Vec3(0, mesh_source_test::TestInfinity(), 0)));
    // 0: 풍부한 표본(16비트), 1: 같은 표본(8비트·상한 4), 2: 가중치 없음(MissingInfluences가 경고를
    // 처음 할당), 3: 정규화 경고가 처음 할당, 4: 보존 설정 경고가 처음 할당, 5·6: morph(정점·코너)
    // 경고가 처음.
    for (uint32 variant = 0; variant < 7; ++variant) {
        const MeshSource* mesh = variant < 2    ? &rich
                                 : variant == 3 ? &normalized
                                 : variant == 5 ? &morphOnly
                                 : variant == 6 ? &normalOnly
                                                : &quad;
        SkinnedMeshBuildInput input = SkinInputFor(*mesh, bones);
        input.settings.useHighPrecisionSkinWeights = variant == 0;
        input.settings.boneInfluenceLimit = variant == 1 ? 4 : 0;
        input.settings.generateLightmapUVs = variant != 4;
        SkinnedMeshBuildResult expected(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, expected));
        const Array<uint8> golden = SkinBytes(fixture.allocator, expected);
        REQUIRE(!golden.IsEmpty());
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(fixture.allocator);
            SkinnedMeshBuildResult built(failing);
            failing.FailOnAttempt(attempt);
            auto result = BuildSkinnedMeshLod(input, built);
            const bool reached = failing.Attempts() >= attempt;
            if (!result) {
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                CHECK(built.mesh.VertexCount() == 0 && built.mesh.warnings.IsEmpty() &&
                      built.influences.IsEmpty() && built.morphTargets.IsEmpty());
                ++failures;
                continue;
            }

            CHECK(SameBytes(golden, SkinBytes(fixture.allocator, built)));
            if (!reached) {
                break;
            }
        }

        CHECK(failures > 20);
        printf("SkinnedMeshBuild_AllocationFailures variant %u: %u failures\n", variant, failures);
    }
}

// 고정 seed 퍼징: MA1 무작위 메시·속성(가중치·morph 포함)에 무작위 설정. 원본 검증이 거부하면 같은
// 진단으로 실패하고, 통과하면 결과 불변식(영향 칸·합·정렬·bone map·morph 정점 순서)을 지키며 두 번
// 빌드한 byte가 같다.
TEST(SkinnedMeshBuild_FuzzSourceMeshes) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, SkinFuzzBones, bones));
    RandomStream random(SkinFuzzSeed);
    uint32 accepted = 0;
    uint32 rejected = 0;
    uint32 morphDeltas = 0;
    for (uint32 iteration = 0; iteration < SkinFuzzMeshCount; ++iteration) {
        MeshSource mesh(fixture.allocator);
        REQUIRE(mesh_source_test::RandomFuzzMesh(random, mesh));
        REQUIRE(mesh_source_test::RandomFuzzAttributes(random, mesh));
        SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
        input.settings.boneInfluenceLimit = static_cast<int32>(random.NextUInt32Below(10));
        input.settings.useHighPrecisionSkinWeights = random.NextUInt32Below(2) == 0;
        input.settings.removeDegenerates = random.NextUInt32Below(2) == 0;
        input.settings.recomputeNormals = random.NextUInt32Below(2) == 0;
        input.settings.thresholdPosition = random.NextUInt32Below(3) == 0 ? 1.0f : 0.00002f;
        MeshValidateOptions options;
        options.boneCount = SkinFuzzBones;
        MeshDiagnostic validation;
        const bool valid = ValidateMeshSource(mesh, options, &validation).IsOk();
        SkinnedMeshBuildResult built(fixture.allocator);
        MeshDiagnostic failure;
        auto result = BuildSkinnedMeshLod(input, built, &failure);
        if (!valid) {
            ++rejected;
            CHECK(!result && failure.code == validation.code && failure.issue == validation.issue);
            continue;
        }

        if (!result) {
            // 원본 검증을 통과해도 값 정리가 가중치 합 0을 거부한다.
            CHECK(failure.issue == MeshIssue::ZeroWeightSum);
            ++rejected;
            continue;
        }

        ++accepted;
        const uint32 vertices = built.mesh.VertexCount();
        CHECK(built.influences.Size() == static_cast<usize>(vertices) * built.influencesPerVertex);
        for (usize at = 1; at < built.boneMap.Size(); ++at) {
            CHECK(built.boneMap[at - 1] < built.boneMap[at]);
        }

        for (uint32 vertex = 0; vertex < vertices; ++vertex) {
            CHECK(InfluencesWellFormed(built, VertexInfluences(built, built.influences, vertex)));
        }

        for (const MorphTargetBuild& target : built.morphTargets) {
            for (usize at = 0; at < target.deltas.Size(); ++at) {
                CHECK(target.deltas[at].vertex < vertices);
                CHECK(at == 0 || target.deltas[at - 1].vertex < target.deltas[at].vertex);
            }

            morphDeltas += static_cast<uint32>(target.deltas.Size());
        }

        SkinnedMeshBuildResult again(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, again));
        CHECK(SameBytes(SkinBytes(fixture.allocator, built), SkinBytes(fixture.allocator, again)));
    }

    printf("SkinnedMeshBuild_FuzzSourceMeshes: %u accepted, %u rejected, %u morph deltas\n",
           accepted, rejected, morphDeltas);
    CHECK(accepted > 100 && rejected > 50);
}

} // namespace skinned_mesh_builder_robustness_tests_file
