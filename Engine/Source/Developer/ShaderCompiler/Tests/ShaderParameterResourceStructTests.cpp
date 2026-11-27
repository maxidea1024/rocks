#include "ShaderManagerTestSupport.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderParameterTestFixtures/ShaderResourceFixtures.shaderparams.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 생성 셰이더 파라미터 구조체의 자원 멤버 시험이다(Docs/Design/ShaderSystem 16.2절 SSD66 1).
// fixture 자원 구조체(ShaderResourceFixtures.h)의 생성 자원 목록이 C++ 배치와 같고, 생성 HLSL의
// 전역 자원·짝 샘플러 선언이 golden과 같으며, 형식 정의 전부를 실제 워커로 컴파일했을 때 값
// 1(바인딩 layout v5)의 자원 항목마다 구조체 identity·멤버 오프셋이 표와 같고 모양을 바꾼 표는
// 거부되는지 본다.
using namespace rocks;

namespace shader_parameter_resource_struct_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;
using namespace rocks::shader_manager_test;
using shader_test_fixtures::FixtureComposite;
using shader_test_fixtures::FixtureMaterial;
using shader_test_fixtures::FixtureReduce;
using shader_test_fixtures::FixtureSceneTextures;
using Kind = ShaderParameterResourceKind;

constexpr const ShaderParameterLayout& MaterialLayout = ShaderParameterLayoutOf<FixtureMaterial>();
constexpr const ShaderParameterLayout& SceneLayout =
    ShaderParameterLayoutOf<FixtureSceneTextures>();
constexpr const ShaderParameterLayout& ReduceLayout = ShaderParameterLayoutOf<FixtureReduce>();
constexpr const ShaderParameterLayout& CompositeLayout =
    ShaderParameterLayoutOf<FixtureComposite>();

// 이름의 자원 멤버다(없으면 null).
const ShaderParameterResource* FindResource(const ShaderParameterLayout& layout,
                                            StringView name) noexcept {
    for (const ShaderParameterResource& resource : layout.resources) {
        if (resource.name == name) {
            return &resource;
        }
    }

    return nullptr;
}

// 자원 하나의 표 칸을 본다.
bool IsResource(const ShaderParameterLayout& layout, StringView name, Kind kind, usize offset,
                ShaderResourceShape shape, ShaderComponentType component, uint32 stride,
                ShaderParameterSampler sampler) noexcept {
    const ShaderParameterResource* found = FindResource(layout, name);
    if (found == nullptr) {
        fprintf(stderr, "missing resource %.*s\n", static_cast<int>(name.ByteCount()), name.Data());
        return false;
    }

    return found->kind == kind && found->offset == offset && found->shape == shape &&
           found->component == component && found->structureStride == stride &&
           found->sampler == sampler &&
           found->samplerName.IsEmpty() == (sampler == ShaderParameterSampler::None);
}

TEST(ShaderParameterStructs_ResourceTablesMatchCpp) {
    using Shape = ShaderResourceShape;
    using Component = ShaderComponentType;
    using Pair = ShaderParameterSampler;
    // 상수 부분: cbuffer 크기(16 byte 배수)이고 C++ 구조체의 앞부분이다. 첫 자원은 그 뒤다.
    CHECK(MaterialLayout.totalBytes == 32);
    CHECK(MaterialLayout.members.Size() == 2);
    CHECK(sizeof(FixtureMaterial) > MaterialLayout.totalBytes);
    CHECK(ROCKS_OFFSET_OF(FixtureMaterial, albedo) >= MaterialLayout.totalBytes);
    CHECK(SceneLayout.totalBytes == 0 && SceneLayout.members.IsEmpty());
    CHECK(CompositeLayout.totalBytes == 0);
    CHECK(ReduceLayout.name == "FixtureReduceConstants" && ReduceLayout.totalBytes == 16);

    // 장치 텍스처(짝·정수 텍스처의 짝 없음·비교 짝·MS), 장치 버퍼, 포함 구조체의 펼친 이름·오프셋.
    CHECK(IsResource(MaterialLayout, "albedo", Kind::DeviceTexture,
                     ROCKS_OFFSET_OF(FixtureMaterial, albedo), Shape::Texture2D, Component::Float,
                     0, Pair::Filter));
    CHECK(FindResource(MaterialLayout, "albedo")->samplerName == "albedoSampler");
    CHECK(IsResource(MaterialLayout, "codes", Kind::DeviceTexture,
                     ROCKS_OFFSET_OF(FixtureMaterial, codes), Shape::Texture2DArray,
                     Component::Uint, 0, Pair::None));
    CHECK(IsResource(MaterialLayout, "shadow", Kind::DeviceTexture,
                     ROCKS_OFFSET_OF(FixtureMaterial, shadow), Shape::Texture2D, Component::Float,
                     0, Pair::Comparison));
    CHECK(IsResource(MaterialLayout, "coverage", Kind::DeviceTexture,
                     ROCKS_OFFSET_OF(FixtureMaterial, coverage), Shape::Texture2DMultisample,
                     Component::Float, 0, Pair::None));
    CHECK(IsResource(MaterialLayout, "lights", Kind::DeviceBuffer,
                     ROCKS_OFFSET_OF(FixtureMaterial, lights), Shape::TypedBuffer, Component::Float,
                     0, Pair::None));
    const usize scene = ROCKS_OFFSET_OF(FixtureMaterial, scene);
    CHECK(IsResource(MaterialLayout, "scene_depth", Kind::GraphTexture,
                     scene + ROCKS_OFFSET_OF(FixtureSceneTextures, depth), Shape::Texture2D,
                     Component::Float, 0, Pair::Filter));
    CHECK(FindResource(MaterialLayout, "scene_depth")->samplerName == "scene_depthSampler");
    CHECK(IsResource(MaterialLayout, "scene_color", Kind::GraphTextureSrv,
                     scene + ROCKS_OFFSET_OF(FixtureSceneTextures, color), Shape::Texture2D,
                     Component::Float, 0, Pair::Filter));
    CHECK(MaterialLayout.resources.Size() == 7);
    CHECK(SceneLayout.resources.Size() == 2);
    CHECK(FindResource(SceneLayout, "depth")->offset ==
          ROCKS_OFFSET_OF(FixtureSceneTextures, depth));

    // 그래프 SRV·UAV(image 형식)·구조화·raw·typed UAV·접근만 선언.
    CHECK(IsResource(ReduceLayout, "source", Kind::GraphTextureSrv,
                     ROCKS_OFFSET_OF(FixtureReduce, source), Shape::Texture2D, Component::Float, 0,
                     Pair::Filter));
    CHECK(IsResource(ReduceLayout, "target", Kind::GraphTextureUav,
                     ROCKS_OFFSET_OF(FixtureReduce, target), Shape::Texture2D, Component::Float, 0,
                     Pair::None));
    CHECK(IsResource(ReduceLayout, "bones", Kind::GraphBufferSrv,
                     ROCKS_OFFSET_OF(FixtureReduce, bones), Shape::StructuredBuffer,
                     Component::None, 64, Pair::None));
    CHECK(IsResource(ReduceLayout, "words", Kind::GraphBufferSrv,
                     ROCKS_OFFSET_OF(FixtureReduce, words), Shape::RawBuffer, Component::None, 0,
                     Pair::None));
    CHECK(IsResource(ReduceLayout, "counters", Kind::GraphBufferUav,
                     ROCKS_OFFSET_OF(FixtureReduce, counters), Shape::StructuredBuffer,
                     Component::None, 4, Pair::None));
    CHECK(IsResource(ReduceLayout, "histogram", Kind::GraphBufferUav,
                     ROCKS_OFFSET_OF(FixtureReduce, histogram), Shape::TypedBuffer, Component::Uint,
                     0, Pair::None));
    CHECK(IsResource(ReduceLayout, "history", Kind::GraphTextureAccess,
                     ROCKS_OFFSET_OF(FixtureReduce, history), Shape::None, Component::None, 0,
                     Pair::None));
    CHECK(IsResource(ReduceLayout, "arguments", Kind::GraphBufferAccess,
                     ROCKS_OFFSET_OF(FixtureReduce, arguments), Shape::None, Component::None, 0,
                     Pair::None));
    CHECK(IsResource(CompositeLayout, "targets", Kind::RenderTargets,
                     ROCKS_OFFSET_OF(FixtureComposite, targets), Shape::None, Component::None, 0,
                     Pair::None));
    CHECK(IsResource(CompositeLayout, "weights", Kind::GraphBufferSrv,
                     ROCKS_OFFSET_OF(FixtureComposite, weights), Shape::TypedBuffer,
                     Component::Float, 0, Pair::None));

    // 표 규칙·목록 규칙을 통과하고 정규 byte로 왕복한다.
    const ShaderParameterLayout layouts[] = {MaterialLayout, ReduceLayout};
    CHECK(ValidateShaderParameterLayouts(layouts));
    CHECK(ValidateShaderParameterLayout(SceneLayout));
    CHECK(ValidateShaderParameterLayout(CompositeLayout));
    Array<uint8> bytes(TestAllocator());
    REQUIRE(EncodeShaderParameterLayouts(layouts, bytes));
    ShaderParameterLayoutSet decoded(TestAllocator());
    REQUIRE(DecodeShaderParameterLayouts(bytes.View(), decoded));
    REQUIRE(decoded.layouts.Size() == 2);
    CHECK(decoded.layouts[0].resources.Size() == MaterialLayout.resources.Size());
    CHECK(decoded.layouts[1].resources.Size() == ReduceLayout.resources.Size());
    CHECK(ShaderParameterLayoutSignature(decoded.View()) ==
          ShaderParameterLayoutSignature(layouts));
}

TEST(ShaderParameterStructs_ResourceHlslGolden) {
    constexpr StringView SceneGolden =
        "// Generated by Rocks.MetaCompiler from "
        "ShaderParameterTestFixtures/ShaderResourceFixtures.h "
        "(rocks::shader_test_fixtures::FixtureSceneTextures). Do not edit.\n"
        "#pragma once\n"
        "\n"
        "Texture2D<float> depth;\n"
        "SamplerState depthSampler;\n"
        "Texture2D<float4> color;\n"
        "SamplerState colorSampler;\n";
    constexpr StringView MaterialGolden =
        "// Generated by Rocks.MetaCompiler from "
        "ShaderParameterTestFixtures/ShaderResourceFixtures.h "
        "(rocks::shader_test_fixtures::FixtureMaterial). Do not edit.\n"
        "#pragma once\n"
        "\n"
        "cbuffer FixtureMaterial {\n"
        "    float4 tint;\n"
        "    float roughness;\n"
        "};\n"
        "\n"
        "Texture2D<float4> albedo;\n"
        "SamplerState albedoSampler;\n"
        "Texture2DArray<uint4> codes;\n"
        "Texture2D<float> shadow;\n"
        "SamplerComparisonState shadowSampler;\n"
        "Texture2DMS<float4> coverage;\n"
        "Buffer<float4> lights;\n"
        "Texture2D<float> scene_depth;\n"
        "SamplerState scene_depthSampler;\n"
        "Texture2D<float4> scene_color;\n"
        "SamplerState scene_colorSampler;\n";
    constexpr StringView ReduceGolden =
        "// Generated by Rocks.MetaCompiler from "
        "ShaderParameterTestFixtures/ShaderResourceFixtures.h "
        "(rocks::shader_test_fixtures::FixtureReduce). Do not edit.\n"
        "#pragma once\n"
        "\n"
        "cbuffer FixtureReduceConstants {\n"
        "    uint4 size;\n"
        "};\n"
        "\n"
        "Texture2D<float4> source;\n"
        "SamplerState sourceSampler;\n"
        "ROCKS_IMAGE_FORMAT(r32f) RWTexture2D<float> target;\n"
        "StructuredBuffer<float4x4> bones;\n"
        "ByteAddressBuffer words;\n"
        "RWStructuredBuffer<uint> counters;\n"
        "ROCKS_IMAGE_FORMAT(r32ui) RWBuffer<uint> histogram;\n";
    constexpr StringView CompositeGolden =
        "// Generated by Rocks.MetaCompiler from "
        "ShaderParameterTestFixtures/ShaderResourceFixtures.h "
        "(rocks::shader_test_fixtures::FixtureComposite). Do not edit.\n"
        "#pragma once\n"
        "\n"
        "Texture2D<float4> source;\n"
        "SamplerState sourceSampler;\n"
        "Texture2D<float4> overlay;\n"
        "SamplerState overlaySampler;\n"
        "Buffer<float> weights;\n";
    const struct {
        const ShaderParameterLayout& layout;
        StringView golden;
    } cases[] = {{SceneLayout, SceneGolden},
                 {MaterialLayout, MaterialGolden},
                 {ReduceLayout, ReduceGolden},
                 {CompositeLayout, CompositeGolden}};
    for (const auto& item : cases) {
        if (item.layout.source != item.golden) {
            fprintf(stderr, "%.*s\n", static_cast<int>(item.layout.source.ByteCount()),
                    item.layout.source.Data());
        }

        CHECK(item.layout.source == item.golden);
    }
}

// 시험 셰이더다: 모든 자원 멤버를 읽어 컴파일러가 지우지 않게 한다(PS는 장치·그래프 SRV, CS는
// 그래프 SRV·UAV·구조화·raw).
constexpr StringView MaterialShader =
    "#include \"/Engine/Generated/ShaderParameterTestFixtures/FixtureMaterial.rsh\"\n"
    "float4 MaterialPixel(float4 position : SV_Position) : SV_Target0 {\n"
    "    const float2 uv = position.xy * 0.01;\n"
    "    float4 sum = tint * roughness;\n"
    "    sum += albedo.Sample(albedoSampler, uv);\n"
    "    sum += float4(codes.Load(int4(0, 0, 0, 0)));\n"
    "    sum += shadow.SampleCmpLevelZero(shadowSampler, uv, 0.5);\n"
    "    sum += coverage.Load(int2(0, 0), 0);\n"
    "    sum += lights.Load(0);\n"
    "    sum += scene_depth.Sample(scene_depthSampler, uv);\n"
    "    sum += scene_color.Sample(scene_colorSampler, uv);\n"
    "    return sum;\n"
    "}\n";

constexpr StringView ReduceShader =
    "#include \"/Engine/Generated/ShaderParameterTestFixtures/FixtureReduce.rsh\"\n"
    "[numthreads(8, 8, 1)]\n"
    "void ReduceCompute(uint3 id : SV_DispatchThreadID) {\n"
    "    float4 value = source.SampleLevel(sourceSampler, float2(id.xy) / float2(size.xy), 0);\n"
    "    value += bones[id.x][0];\n"
    "    value.x += asfloat(words.Load(id.x * 4));\n"
    "    target[id.xy] = value.x;\n"
    "    counters[id.x] = id.y;\n"
    "    histogram[id.x] = id.x + size.z;\n"
    "}\n";

// 값 1의 상수 버퍼 밖 자원마다 구조체 identity·멤버 오프셋이 표와 같은지 본다. 셰이더가 쓰는 자원
// 멤버 수(짝 샘플러 포함)를 matched에 둔다.
void CheckResources(StringView id, const ShaderParameterLayout& layout,
                    const ShaderCompileJobResult& result, usize& matched) {
    ShaderBindingLayout binding(TestAllocator());
    REQUIRE(DecodeShaderBindingLayout(result.values.Part(ShaderNuggetValue::BindingLayout).View(),
                                      binding));
    matched = 0;
    for (const ShaderBindingResource& resource : binding.resources) {
        if (resource.kind == ShaderResourceKind::ConstantBuffer) {
            CHECK(resource.memberOffset == NoShaderParameterMember);
            continue;
        }

        const ShaderParameterResource* member = nullptr;
        for (const ShaderParameterResource& candidate : layout.resources) {
            const StringView name = resource.kind == ShaderResourceKind::Sampler
                                        ? candidate.samplerName
                                        : candidate.name;
            member = !name.IsEmpty() && name == resource.name ? &candidate : member;
        }

        if (member == nullptr) {
            fprintf(stderr, "%.*s: resource %.*s is not a member\n",
                    static_cast<int>(id.ByteCount()), id.Data(),
                    static_cast<int>(resource.name.ByteCount()), resource.name.Data());
        }

        REQUIRE(member != nullptr);
        CHECK(resource.structId == layout.structId);
        CHECK(resource.memberOffset == member->offset);
        if (resource.kind != ShaderResourceKind::Sampler) {
            CHECK(resource.shape == member->shape);
            CHECK(resource.component == member->component);
        }

        ++matched;
    }
}

// 첫 자원의 모양을 바꾼 표(워커가 거부해야 한다)다.
struct TamperedLayout {
    explicit TamperedLayout(const ShaderParameterLayout& original, StringView target) noexcept
        : layout(original) {
        for (usize index = 0; index < original.resources.Size() && index < Capacity; ++index) {
            resources[index] = original.resources[index];
            if (resources[index].name == target) {
                resources[index].shape = resources[index].shape == ShaderResourceShape::Texture2D
                                             ? ShaderResourceShape::Texture2DArray
                                             : ShaderResourceShape::RawBuffer;
                resources[index].structureStride = 0;
            }
        }

        layout = ShaderParameterLayout(
            original.name, original.rootConstants, original.totalBytes, original.members,
            Span<const ShaderParameterResource>(resources, original.resources.Size()),
            original.structName, original.sourcePath, original.source);
    }

    static constexpr usize Capacity = 16;
    ShaderParameterResource resources[Capacity];
    ShaderParameterLayout layout;
};

TEST(ShaderParameterStructs_ResourceCompileMatrix) {
    char worker[ManagerPathBytes];
    if (!WorkerExecutable(worker)) {
        printf("[SKIP] ShaderParameterStructs_ResourceCompileMatrix: ShaderCompileWorker is not "
               "next to the test executable\n");
        return;
    }

    Fixture fixture;
    REQUIRE(fixture.Make("shaderparamresources"));
    if (!ToolsReady(worker, fixture)) {
        printf("[SKIP] ShaderParameterStructs_ResourceCompileMatrix: shader tool store is not "
               "bootstrapped\n");
        return;
    }

    REQUIRE(fixture.MakeDirectory("Tests"));
    REQUIRE(fixture.Write("Tests/Material.rsf", MaterialShader));
    REQUIRE(fixture.Write("Tests/Reduce.rsf", ReduceShader));
    char engine[ManagerPathBytes];
    REQUIRE(RepositoryShaderDirectory(engine, sizeof(engine)));
    char tests[ManagerPathBytes];
    REQUIRE(snprintf(tests, sizeof(tests), "%s/Tests", fixture.Path()) > 0);
    ShaderSourceTree tree(TestAllocator());
    REQUIRE(tree.Mount("/Engine", StringView(engine)));
    REQUIRE(tree.Mount("/Plugin/ShaderParameterTests", StringView(tests)));
    KeyEnv env;
    REQUIRE(env.registered);

    REQUIRE(MaterialLayout.resources.Size() <= TamperedLayout::Capacity &&
            ReduceLayout.resources.Size() <= TamperedLayout::Capacity);
    const TamperedLayout wrongMaterial(MaterialLayout, "albedo");
    const TamperedLayout wrongReduce(ReduceLayout, "bones");
    const ShaderParameterLayout material[] = {MaterialLayout};
    const ShaderParameterLayout reduce[] = {ReduceLayout};
    const ShaderParameterLayout materialTampered[] = {wrongMaterial.layout};
    const ShaderParameterLayout reduceTampered[] = {wrongReduce.layout};
    // job 순서: 형식마다 PS 정상, PS 변조, CS 정상, CS 변조.
    constexpr usize JobsPerFormat = 4;
    Array<UniquePtr<ShaderFormatSnapshot>> snapshots(TestAllocator());
    Array<ShaderCompileJob> jobs(TestAllocator());
    for (const StringView id : ShaderFormatIds) {
        auto snapshot = UniquePtr<ShaderFormatSnapshot>::TryMake(TestAllocator(), TestAllocator());
        REQUIRE(snapshot);
        REQUIRE(LoadShaderFormatSnapshot(ShaderFormatDirectory, id, *snapshot.Value()));
        const struct {
            StringView path;
            StringView entry;
            uint32 stage;
            Span<const ShaderParameterLayout> layouts;
        } cases[JobsPerFormat] = {{"/Plugin/ShaderParameterTests/Material.rsf", "MaterialPixel",
                                   ShaderStagePixel, material},
                                  {"/Plugin/ShaderParameterTests/Material.rsf", "MaterialPixel",
                                   ShaderStagePixel, materialTampered},
                                  {"/Plugin/ShaderParameterTests/Reduce.rsf", "ReduceCompute",
                                   ShaderStageCompute, reduce},
                                  {"/Plugin/ShaderParameterTests/Reduce.rsf", "ReduceCompute",
                                   ShaderStageCompute, reduceTampered}};
        for (const auto& item : cases) {
            ShaderCompileJob job;
            job.format = snapshot.Value().Get();
            job.entryPath = item.path;
            job.entryPoint = item.entry;
            job.stage = item.stage;
            job.options = DefaultShaderCompileOptions(snapshot.Value()->definition);
            job.parameterLayouts = item.layouts;
            REQUIRE(jobs.TryPushBack(job));
        }

        REQUIRE(snapshots.TryPushBack(Move(snapshot.Value())));
    }

    ShaderCompileManagerConfig config;
    char work[ManagerPathBytes];
    REQUIRE(snprintf(work, sizeof(work), "%s/Work", fixture.Path()) > 0);
    config.allocator = &TestAllocator();
    config.tree = &tree;
    config.reflection = env.Reflection();
    config.workerExecutable = StringView(worker);
    config.workRoot = StringView(work);
    ShaderCompileManager manager(config);
    REQUIRE(manager.Start());
    Array<ShaderCompileJobResult> results(TestAllocator());
    REQUIRE(manager.Compile(jobs.View(), results));
    REQUIRE(results.Size() == jobs.Size());
    for (usize format = 0; format < snapshots.Size(); ++format) {
        const StringView id = ShaderFormatIds[format];
        const ShaderCompileJobResult* group = &results[format * JobsPerFormat];
        if (snapshots[format]->definition.stages == 0 ||
            group[0].status == ShaderCompileStatus::UnsupportedCapability ||
            group[0].status == ShaderCompileStatus::ToolError) {
            printf("[SKIP] ShaderParameterStructs_ResourceCompileMatrix %.*s: %s\n",
                   static_cast<int>(id.ByteCount()), id.Data(),
                   snapshots[format]->definition.stages == 0
                       ? "placeholder format"
                       : ShaderCompileStatusName(group[0].status));
            continue;
        }

        for (usize job = 0; job < JobsPerFormat; job += 2) {
            const ShaderCompileJobResult& good = group[job];
            const ShaderCompileJobResult& bad = group[job + 1];
            if (good.status != ShaderCompileStatus::Success) {
                fprintf(stderr, "%.*s job %zu: %s\n%.*s\n", static_cast<int>(id.ByteCount()),
                        id.Data(), job, ShaderCompileStatusName(good.status),
                        static_cast<int>(good.diagnostics.ByteCount()), good.diagnostics.Data());
            }

            REQUIRE(good.status == ShaderCompileStatus::Success);
            const ShaderParameterLayout& layout = job == 0 ? MaterialLayout : ReduceLayout;
            // PS: 자원 7 + 짝 샘플러 4(albedo·shadow·scene_depth·scene_color). CS: 자원 6 + 짝 1.
            usize matched = 0;
            CheckResources(id, layout, good, matched);
            CHECK(matched == (job == 0 ? 11u : 7u));
            if (bad.status != ShaderCompileStatus::CompileError) {
                fprintf(stderr, "%.*s job %zu tampered: %s\n", static_cast<int>(id.ByteCount()),
                        id.Data(), job + 1, ShaderCompileStatusName(bad.status));
            }

            CHECK(bad.status == ShaderCompileStatus::CompileError);
            CHECK(bad.diagnostics.View().Contains(StringView(job == 0 ? "albedo" : "bones")));
        }

        printf("  ShaderParameterStructs_ResourceCompileMatrix %.*s: resource members match, "
               "tampered tables rejected\n",
               static_cast<int>(id.ByteCount()), id.Data());
    }
}

} // namespace shader_parameter_resource_struct_tests_file
