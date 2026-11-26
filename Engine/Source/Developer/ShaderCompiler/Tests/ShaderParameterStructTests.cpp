#include "ShaderManagerTestSupport.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderParameterTestFixtures/ShaderParameterFixtures.shaderparams.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 생성 셰이더 파라미터 구조체 시험이다(Docs/Design/ShaderSystem 16.2절 SSD58 1, 7.3절 (a)·(f)를
// 생성 구조체로). fixture 구조체(ShaderParameterTestFixtures)의 생성 표가 C++ 배치와 같고, 생성
// HLSL이 golden과 같으며, 형식 정의 전부를 실제 워커로 컴파일했을 때 리플렉션 오프셋 = 표 오프셋 =
// C++ 오프셋(워커 대조가 통과하고 값 1 멤버·GLES 복사 표가 표와 같다)인지 본다.
using namespace rocks;

namespace shader_parameter_struct_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;
using namespace rocks::shader_manager_test;
using shader_test_fixtures::FixtureDraw;
using shader_test_fixtures::FixtureLight;
using shader_test_fixtures::FixtureScene;

constexpr const ShaderParameterLayout& SceneLayout = ShaderParameterLayoutOf<FixtureScene>();
constexpr const ShaderParameterLayout& LightLayout = ShaderParameterLayoutOf<FixtureLight>();
constexpr const ShaderParameterLayout& DrawLayout = ShaderParameterLayoutOf<FixtureDraw>();

// 이름의 멤버다(없으면 null).
const ShaderParameterMember* FindMember(const ShaderParameterLayout& layout,
                                        StringView name) noexcept {
    for (const ShaderParameterMember& member : layout.members) {
        if (member.name == name) {
            return &member;
        }
    }

    return nullptr;
}

#define CHECK_MEMBER(layout, Type, field, kind)                                                    \
    do {                                                                                           \
        const ShaderParameterMember* found = FindMember(layout, StringView(#field));               \
        REQUIRE(found != nullptr);                                                                 \
        CHECK(found->offset == ROCKS_OFFSET_OF(Type, field));                                      \
        CHECK(found->type == ShaderParameterType::kind);                                           \
    } while (false)

TEST(ShaderParameterStructs_GeneratedTablesMatchCpp) {
    // 표 머리: HLSL 이름·루트 상수·크기·identity·원문 경로.
    CHECK(SceneLayout.name == "FixtureScene");
    CHECK(!SceneLayout.rootConstants);
    CHECK(SceneLayout.totalBytes == sizeof(FixtureScene));
    CHECK(SceneLayout.totalBytes == 256);
    CHECK(SceneLayout.structName == "rocks::shader_test_fixtures::FixtureScene");
    CHECK(SceneLayout.structId == ShaderParameterStructId(SceneLayout.structName));
    CHECK(SceneLayout.structId != 0);
    CHECK(SceneLayout.sourcePath ==
          "/Engine/Generated/ShaderParameterTestFixtures/FixtureScene.rsh");
    CHECK(DrawLayout.name == "FixtureRoot");
    CHECK(DrawLayout.rootConstants);
    CHECK(DrawLayout.totalBytes == sizeof(FixtureDraw));
    CHECK(DrawLayout.totalBytes == 112);
    CHECK(LightLayout.totalBytes == sizeof(FixtureLight));

    // 멤버마다 C++ 오프셋과 같다(3성분 뒤 스칼라, int 슬롯, 배열, 중첩, 행렬).
    CHECK_MEMBER(SceneLayout, FixtureScene, worldToClip, Float4x4);
    CHECK_MEMBER(SceneLayout, FixtureScene, tint, Float3);
    CHECK_MEMBER(SceneLayout, FixtureScene, exposure, Float);
    CHECK_MEMBER(SceneLayout, FixtureScene, uvScale, Float2);
    CHECK_MEMBER(SceneLayout, FixtureScene, offsets, Int3);
    CHECK_MEMBER(SceneLayout, FixtureScene, layer, Int);
    CHECK_MEMBER(SceneLayout, FixtureScene, counts, Uint4);
    CHECK_MEMBER(SceneLayout, FixtureScene, weights, Float4);
    CHECK_MEMBER(SceneLayout, FixtureScene, light, Struct);
    CHECK_MEMBER(SceneLayout, FixtureScene, tail, Float);
    CHECK_MEMBER(LightLayout, FixtureLight, direction, Float3);
    CHECK_MEMBER(LightLayout, FixtureLight, intensity, Float);
    CHECK_MEMBER(LightLayout, FixtureLight, color, Float4);
    CHECK_MEMBER(DrawLayout, FixtureDraw, transform, Float4x4);
    CHECK_MEMBER(DrawLayout, FixtureDraw, color, Float3);
    CHECK_MEMBER(DrawLayout, FixtureDraw, alpha, Float);
    CHECK_MEMBER(DrawLayout, FixtureDraw, ids, Uint4);
    CHECK(FindMember(SceneLayout, "weights")->arrayCount == 3);
    CHECK(FindMember(SceneLayout, "light")->structLayout == &LightLayout);

    // 명시 padding: 3성분 다음(같은 기본 타입).
    const ShaderParameterMember* tintPadding = FindMember(SceneLayout, "rocksPadding0");
    REQUIRE(tintPadding != nullptr);
    CHECK(tintPadding->offset == 76);
    CHECK(tintPadding->type == ShaderParameterType::Float);
    // D3D 배치가 당길 수 있는 2성분 앞(84)과 정수 슬롯 끝(116–124)도 채운다.
    const ShaderParameterMember* twoPadding = FindMember(SceneLayout, "rocksPadding1");
    REQUIRE(twoPadding != nullptr);
    CHECK(twoPadding->offset == 84);
    CHECK(twoPadding->type == ShaderParameterType::Float);
    const ShaderParameterMember* intPadding = FindMember(SceneLayout, "rocksPadding2");
    REQUIRE(intPadding != nullptr);
    CHECK(intPadding->offset == 108);
    CHECK(intPadding->type == ShaderParameterType::Int);
    CHECK(FindMember(SceneLayout, "rocksPadding5")->offset == 124);
    CHECK(FindMember(SceneLayout, "rocksPadding5")->type == ShaderParameterType::Int);
    CHECK(FindMember(LightLayout, "rocksPadding0")->offset == 12);
    CHECK(FindMember(LightLayout, "rocksPadding3")->offset == 28);
    CHECK(FindMember(DrawLayout, "rocksPadding0")->offset == 76);
    CHECK(FindMember(DrawLayout, "rocksPadding3")->offset == 92);
    CHECK(SceneLayout.members.Size() == 16);
    CHECK(LightLayout.members.Size() == 7);

    // 표 규칙·목록 규칙을 통과하고 정규 byte로 왕복한다(중첩 표·identity 포함).
    const ShaderParameterLayout layouts[] = {SceneLayout, DrawLayout};
    const char* reason = nullptr;
    CHECK(ValidateShaderParameterLayouts(layouts, &reason));
    Array<uint8> bytes(TestAllocator());
    REQUIRE(EncodeShaderParameterLayouts(layouts, bytes));
    ShaderParameterLayoutSet decoded(TestAllocator());
    REQUIRE(DecodeShaderParameterLayouts(bytes.View(), decoded));
    REQUIRE(decoded.layouts.Size() == 2);
    CHECK(decoded.layouts[0].structId == SceneLayout.structId);
    CHECK(decoded.layouts[0].structName == SceneLayout.structName);
    usize nestedMembers = 0;
    for (const ShaderParameterMember& member : decoded.layouts[0].members) {
        if (member.type == ShaderParameterType::Struct) {
            REQUIRE(member.structLayout != nullptr);
            CHECK(member.structLayout->members.Size() == LightLayout.members.Size());
            CHECK(member.structLayout->structId == LightLayout.structId);
            nestedMembers += member.structLayout->members.Size();
        }
    }

    CHECK(nestedMembers == 7);
    CHECK(ShaderParameterLayoutSignature(decoded.View()) ==
          ShaderParameterLayoutSignature(layouts));

    // identity는 서명에 든다: 같은 표에 다른 구조체 이름이면 서명이 바뀐다.
    ShaderParameterLayout renamed(SceneLayout.name, false, SceneLayout.totalBytes,
                                  SceneLayout.members, "rocks::other::FixtureScene",
                                  SceneLayout.sourcePath, SceneLayout.source);
    const ShaderParameterLayout renamedList[] = {renamed, DrawLayout};
    CHECK(ShaderParameterLayoutSignature(renamedList) != ShaderParameterLayoutSignature(layouts));
}

#undef CHECK_MEMBER

TEST(ShaderParameterStructs_GeneratedHlslGolden) {
    constexpr StringView SceneGolden = "// Generated by Rocks.MetaCompiler from "
                                       "ShaderParameterTestFixtures/ShaderParameterFixtures.h "
                                       "(rocks::shader_test_fixtures::FixtureScene). Do not edit.\n"
                                       "#pragma once\n"
                                       "\n"
                                       "#ifndef ROCKS_SHADER_STRUCT_FixtureLight\n"
                                       "#define ROCKS_SHADER_STRUCT_FixtureLight\n"
                                       "struct FixtureLight {\n"
                                       "    float3 direction;\n"
                                       "    float rocksPadding0;\n"
                                       "    float intensity;\n"
                                       "    float rocksPadding1;\n"
                                       "    float rocksPadding2;\n"
                                       "    float rocksPadding3;\n"
                                       "    float4 color;\n"
                                       "};\n"
                                       "#endif\n"
                                       "\n"
                                       "cbuffer FixtureScene {\n"
                                       "    float4x4 worldToClip;\n"
                                       "    float3 tint;\n"
                                       "    float rocksPadding0;\n"
                                       "    float exposure;\n"
                                       "    float rocksPadding1;\n"
                                       "    float2 uvScale;\n"
                                       "    int3 offsets;\n"
                                       "    int rocksPadding2;\n"
                                       "    int layer;\n"
                                       "    int rocksPadding3;\n"
                                       "    int rocksPadding4;\n"
                                       "    int rocksPadding5;\n"
                                       "    uint4 counts;\n"
                                       "    float4 weights[3];\n"
                                       "    FixtureLight light;\n"
                                       "    float tail;\n"
                                       "};\n";
    constexpr StringView DrawGolden = "// Generated by Rocks.MetaCompiler from "
                                      "ShaderParameterTestFixtures/ShaderParameterFixtures.h "
                                      "(rocks::shader_test_fixtures::FixtureDraw). Do not edit.\n"
                                      "#pragma once\n"
                                      "\n"
                                      "#ifndef ROCKS_SHADER_STRUCT_FixtureDraw\n"
                                      "#define ROCKS_SHADER_STRUCT_FixtureDraw\n"
                                      "struct FixtureDraw {\n"
                                      "    float4x4 transform;\n"
                                      "    float3 color;\n"
                                      "    float rocksPadding0;\n"
                                      "    float alpha;\n"
                                      "    float rocksPadding1;\n"
                                      "    float rocksPadding2;\n"
                                      "    float rocksPadding3;\n"
                                      "    uint4 ids;\n"
                                      "};\n"
                                      "#endif\n"
                                      "\n"
                                      "ROCKS_ROOT_CONSTANTS(FixtureDraw, FixtureRoot);\n";
    constexpr StringView LightGolden = "// Generated by Rocks.MetaCompiler from "
                                       "ShaderParameterTestFixtures/ShaderParameterFixtures.h "
                                       "(rocks::shader_test_fixtures::FixtureLight). Do not edit.\n"
                                       "#pragma once\n"
                                       "\n"
                                       "cbuffer FixtureLight {\n"
                                       "    float3 direction;\n"
                                       "    float rocksPadding0;\n"
                                       "    float intensity;\n"
                                       "    float rocksPadding1;\n"
                                       "    float rocksPadding2;\n"
                                       "    float rocksPadding3;\n"
                                       "    float4 color;\n"
                                       "};\n";
    if (SceneLayout.source != SceneGolden) {
        fprintf(stderr, "%.*s\n", static_cast<int>(SceneLayout.source.ByteCount()),
                SceneLayout.source.Data());
    }

    CHECK(SceneLayout.source == SceneGolden);
    CHECK(DrawLayout.source == DrawGolden);
    CHECK(LightLayout.source == LightGolden);
}

// 시험 셰이더다: 모든 멤버를 읽어 두 버퍼가 컴파일러에 지워지지 않게 한다.
constexpr StringView FixtureShader =
    "#include \"/Engine/Generated/ShaderParameterTestFixtures/FixtureScene.rsh\"\n"
    "#include \"/Engine/Generated/ShaderParameterTestFixtures/FixtureDraw.rsh\"\n"
    "float4 FixturePixel(float4 position : SV_Position) : SV_Target0 {\n"
    "    float4 sum = mul(float4(tint, exposure), worldToClip);\n"
    "    sum.xy += uvScale + float2(offsets.xy) + float2(offsets.z + layer, 0.0);\n"
    "    sum += float4(counts) + weights[0] + weights[1] + weights[2];\n"
    "    sum += light.color * light.intensity + float4(light.direction, tail);\n"
    "    sum += mul(float4(FixtureRoot.color, FixtureRoot.alpha), FixtureRoot.transform);\n"
    "    sum += float4(FixtureRoot.ids);\n"
    "    return sum + position;\n"
    "}\n";

constexpr StringView Formats[] = {"D3D12_SM6", "D3D11_SM5",      "VULKAN_SM5", "VULKAN_ANDROID",
                                  "GLES31_PC", "GLES31_ANDROID", "METAL_SM5",  "METAL_IOS"};

// 형식 하나의 결과를 대조한다(값 1 identity·멤버, GLES 복사 표).
void CheckFormatResult(StringView id, const ShaderCompileJobResult& result) {
    ShaderBindingLayout binding(TestAllocator());
    REQUIRE(DecodeShaderBindingLayout(result.values.Part(ShaderNuggetValue::BindingLayout).View(),
                                      binding));
    CHECK(binding.rootConstantBytes == DrawLayout.totalBytes);
    CHECK(binding.rootConstantStructId == DrawLayout.structId);
    const ShaderBindingResource* scene = nullptr;
    for (const ShaderBindingResource& resource : binding.resources) {
        if (resource.kind == ShaderResourceKind::ConstantBuffer &&
            resource.name == "FixtureScene") {
            scene = &resource;
        }
    }

    REQUIRE(scene != nullptr);
    CHECK(scene->structId == SceneLayout.structId);
    CHECK(scene->constantBufferBytes == SceneLayout.totalBytes);
    const Span<const ShaderBindingMember> members = binding.MembersOf(*scene);
    CHECK(members.Size() == SceneLayout.members.Size());
    for (usize index = 0; index < members.Size() && index < SceneLayout.members.Size(); ++index) {
        CHECK(members[index].name == SceneLayout.members[index].name);
        CHECK(members[index].offset == SceneLayout.members[index].offset);
        // 중첩 구조체의 byte 수는 형식 리플렉션마다 철자가 달라(끝 padding 포함 여부) 보지 않는다.
        if (SceneLayout.members[index].type != ShaderParameterType::Struct) {
            CHECK(members[index].bytes == ShaderParameterMemberBytes(SceneLayout.members[index]));
        }
    }

    if (!result.values.Has(ShaderNuggetValue::GlesCopyTable)) {
        return;
    }

    // GLES: 복사 표가 상수 버퍼 전체(슬롯 16개)와 루트 상수 전체를 덮는다.
    ShaderGlesBindingHeader header(TestAllocator());
    REQUIRE(DecodeShaderGlesBindingHeader(
        result.values.Part(ShaderNuggetValue::GlesCopyTable).View(), header));
    uint64 copied = 0;
    for (const ShaderGlesCopy& copy : header.copies) {
        copied += copy.bytes;
    }

    CHECK(copied == uint64{SceneLayout.totalBytes} + DrawLayout.totalBytes);
    printf("  ShaderParameterStructs_CompileMatrix %.*s: %zu GLES copies\n",
           static_cast<int>(id.ByteCount()), id.Data(), header.copies.Size());
}

TEST(ShaderParameterStructs_CompileMatrix) {
    char worker[ManagerPathBytes];
    if (!WorkerExecutable(worker)) {
        printf("[SKIP] ShaderParameterStructs_CompileMatrix: ShaderCompileWorker is not next to "
               "the test executable\n");
        return;
    }

    Fixture fixture;
    REQUIRE(fixture.Make("shaderparamstructs"));
    if (!ToolsReady(worker, fixture)) {
        printf("[SKIP] ShaderParameterStructs_CompileMatrix: shader tool store is not "
               "bootstrapped\n");
        return;
    }

    REQUIRE(fixture.MakeDirectory("Tests"));
    REQUIRE(fixture.Write("Tests/Fixture.rsf", FixtureShader));
    char engine[ManagerPathBytes];
    REQUIRE(RepositoryShaderDirectory(engine, sizeof(engine)));
    char tests[ManagerPathBytes];
    REQUIRE(snprintf(tests, sizeof(tests), "%s/Tests", fixture.Path()) > 0);
    ShaderSourceTree tree(TestAllocator());
    REQUIRE(tree.Mount("/Engine", StringView(engine)));
    REQUIRE(tree.Mount("/Plugin/ShaderParameterTests", StringView(tests)));
    KeyEnv env;
    REQUIRE(env.registered);

    // 정상 표와, 중첩 구조체 멤버 타입을 틀리게 바꾼 표(워커가 거부해야 한다 — 7.3절 (a) 음성)다.
    const ShaderParameterLayout layouts[] = {SceneLayout, DrawLayout};
    constexpr usize MemberCapacity = 32;
    ShaderParameterMember wrongLight[MemberCapacity];
    const usize lightCount = LightLayout.members.Size();
    REQUIRE(lightCount <= MemberCapacity && SceneLayout.members.Size() <= MemberCapacity);
    for (usize index = 0; index < lightCount; ++index) {
        wrongLight[index] = LightLayout.members[index];
        // color의 타입을 int4로 바꾼다(표 규칙은 지키고 셰이더와는 다르다).
        if (wrongLight[index].name == "color") {
            wrongLight[index].type = ShaderParameterType::Int4;
        }
    }

    const ShaderParameterLayout wrongNested(
        "FixtureLight", false, LightLayout.totalBytes,
        Span<const ShaderParameterMember>(wrongLight, lightCount));
    ShaderParameterMember wrongScene[MemberCapacity];
    for (usize index = 0; index < SceneLayout.members.Size(); ++index) {
        wrongScene[index] = SceneLayout.members[index];
        if (wrongScene[index].type == ShaderParameterType::Struct) {
            wrongScene[index].structLayout = &wrongNested;
        }
    }

    const ShaderParameterLayout tampered[] = {
        ShaderParameterLayout(
            SceneLayout.name, false, SceneLayout.totalBytes,
            Span<const ShaderParameterMember>(wrongScene, SceneLayout.members.Size()),
            SceneLayout.structName, SceneLayout.sourcePath, SceneLayout.source),
        DrawLayout};

    Array<UniquePtr<ShaderFormatSnapshot>> snapshots(TestAllocator());
    Array<ShaderCompileJob> jobs(TestAllocator());
    for (const StringView id : Formats) {
        auto snapshot = UniquePtr<ShaderFormatSnapshot>::TryMake(TestAllocator(), TestAllocator());
        REQUIRE(snapshot);
        REQUIRE(LoadShaderFormatSnapshot(ShaderFormatDirectory, id, *snapshot.Value()));
        for (const Span<const ShaderParameterLayout> list :
             {Span<const ShaderParameterLayout>(layouts),
              Span<const ShaderParameterLayout>(tampered)}) {
            ShaderCompileJob job;
            job.format = snapshot.Value().Get();
            job.entryPath = "/Plugin/ShaderParameterTests/Fixture.rsf";
            job.entryPoint = "FixturePixel";
            job.stage = ShaderStagePixel;
            job.options = DefaultShaderCompileOptions(snapshot.Value()->definition);
            job.parameterLayouts = list;
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
        const StringView id = Formats[format];
        const ShaderCompileJobResult& good = results[format * 2];
        const ShaderCompileJobResult& bad = results[format * 2 + 1];
        if (snapshots[format]->definition.stages == 0 ||
            good.status == ShaderCompileStatus::UnsupportedCapability ||
            good.status == ShaderCompileStatus::ToolError) {
            printf("[SKIP] ShaderParameterStructs_CompileMatrix %.*s: %s\n",
                   static_cast<int>(id.ByteCount()), id.Data(),
                   snapshots[format]->definition.stages == 0
                       ? "placeholder format"
                       : ShaderCompileStatusName(good.status));
            continue;
        }

        if (good.status != ShaderCompileStatus::Success) {
            fprintf(stderr, "%.*s: %s\n%.*s\n", static_cast<int>(id.ByteCount()), id.Data(),
                    ShaderCompileStatusName(good.status),
                    static_cast<int>(good.diagnostics.ByteCount()), good.diagnostics.Data());
        }

        REQUIRE(good.status == ShaderCompileStatus::Success);
        CheckFormatResult(id, good);
        if (bad.status != ShaderCompileStatus::CompileError) {
            fprintf(stderr, "%.*s tampered: %s\n", static_cast<int>(id.ByteCount()), id.Data(),
                    ShaderCompileStatusName(bad.status));
        }

        CHECK(bad.status == ShaderCompileStatus::CompileError);
        CHECK(bad.diagnostics.View().Contains(StringView("FixtureScene")));
        printf("  ShaderParameterStructs_CompileMatrix %.*s: offsets match, tampered table "
               "rejected\n",
               static_cast<int>(id.ByteCount()), id.Data());
    }
}

} // namespace shader_parameter_struct_tests_file
