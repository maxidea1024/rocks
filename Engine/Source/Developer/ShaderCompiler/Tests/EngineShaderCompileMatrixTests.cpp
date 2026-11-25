#include "ShaderManagerTestSupport.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Platform/Process.h>
#include <RenderBase/EngineShaderTypes.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCore/ShaderType.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 엔진 셰이더 SL0 컴파일 행렬(Docs/Design/EngineShaders 7.6·7.7절 1항, 8.1절 (C), 결정 SLD19)이다.
// 시험 셰이더 폴더 Programs/Sandbox/EngineShaderTests를 /Plugin/EngineShaderTests로,
// Engine/Shaders를 /Engine으로 mount하고 실제 ShaderCompileWorker로 시험 엔트리(include 폐포
// VS·PS·CS와 영역 probe)와 엔진 셰이더 타입 전부(RenderBase EngineShaderTypes — SL0 파일을 쓰는
// tone map·텍스처 표시 포함)를 형식 정의 전부에 컴파일한다. 경고는 오류다(워커 -WX·FXC
// WARNINGS_ARE_ERRORS). 이 호스트가 만들 수 없는 형식(Windows의 Metal —
// UnsupportedCapability·ToolError)은 형식마다 SKIP과 사유를 찍는다. 워커가 없거나 도구 저장소가
// 준비되지 않았으면 시험 전체가 SKIP이다.
using namespace rocks;

namespace engine_shader_compile_matrix_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;
using namespace rocks::shader_manager_test;

// 시험 셰이더 폴더(저장소 뿌리 기준)와 가상 루트다.
constexpr char TestShaderFolder[] = "Programs/Sandbox/EngineShaderTests";
constexpr char TestShaderRoot[] = "/Plugin/EngineShaderTests";

// 형식 정의 전부다(Engine/Config/ShaderFormats).
constexpr StringView Formats[] = {"D3D12_SM6", "D3D11_SM5",      "VULKAN_SM5", "VULKAN_ANDROID",
                                  "GLES31_PC", "GLES31_ANDROID", "METAL_SM5",  "METAL_IOS"};

struct TestEntry {
    const char* path;
    const char* entry;
    uint32 stage;
};

// 시험 엔트리다(Sandbox EngineShaderTestTypes.inl과 같은 엔트리 — 이 시험은 레이아웃 표 대조를 하지
// 않는다. 레이아웃 대조는 Sandbox 등록 타입이 장치 형식마다 한다).
constexpr TestEntry TestEntries[] = {
    {"/Plugin/EngineShaderTests/Common/IncludeClosure.rsf", "IncludeClosureVertex",
     ShaderStageVertex},
    {"/Plugin/EngineShaderTests/Common/IncludeClosure.rsf", "IncludeClosurePixel",
     ShaderStagePixel},
    {"/Plugin/EngineShaderTests/Common/IncludeClosure.rsf", "IncludeClosureCompute",
     ShaderStageCompute},
    {"/Plugin/EngineShaderTests/Math/MathProbes.rsf", "MathFloatProbePixel", ShaderStagePixel},
    {"/Plugin/EngineShaderTests/Math/MathProbes.rsf", "MathUintProbePixel", ShaderStagePixel},
    {"/Plugin/EngineShaderTests/Color/ColorProbes.rsf", "ColorFloatProbePixel", ShaderStagePixel},
    {"/Plugin/EngineShaderTests/Shading/ShadingTestKernels.rsf", "ShadingFloatProbePixel",
     ShaderStagePixel}};

// 이 호스트에서 만들 수 없다는 결과인가(형식 SKIP).
bool IsHostUnsupported(const ShaderCompileJobResult& result) noexcept {
    return result.status == ShaderCompileStatus::UnsupportedCapability ||
           result.status == ShaderCompileStatus::ToolError;
}

TEST(EngineShaders_CompileMatrix) {
    char worker[ManagerPathBytes];
    if (!WorkerExecutable(worker)) {
        printf("[SKIP] EngineShaders_CompileMatrix: ShaderCompileWorker is not next to the test "
               "executable\n");
        return;
    }

    Fixture fixture;
    REQUIRE(fixture.Make("enginecompilematrix"));
    if (!ToolsReady(worker, fixture)) {
        printf("[SKIP] EngineShaders_CompileMatrix: shader tool store is not bootstrapped\n");
        return;
    }

    char engine[ManagerPathBytes];
    REQUIRE(RepositoryShaderDirectory(engine, sizeof(engine)));
    char tests[ManagerPathBytes];
    {
        char working[ManagerPathBytes];
        usize bytes = 0;
        REQUIRE(platform::GetStandardDirectory(platform::StandardDirectory::Working, working,
                                               sizeof(working), bytes));
        REQUIRE(snprintf(tests, sizeof(tests), "%s/%s", working, TestShaderFolder) > 0);
    }

    ShaderSourceTree tree(TestAllocator());
    REQUIRE(tree.Mount("/Engine", StringView(engine)));
    REQUIRE(tree.Mount(TestShaderRoot, StringView(tests)));
    KeyEnv env;
    REQUIRE(env.registered);

    // 형식 snapshot이다(형식마다 하나 — job이 가리킨다).
    Array<UniquePtr<ShaderFormatSnapshot>> snapshots(TestAllocator());
    for (const StringView id : Formats) {
        auto snapshot = UniquePtr<ShaderFormatSnapshot>::TryMake(TestAllocator(), TestAllocator());
        REQUIRE(snapshot);
        REQUIRE(LoadShaderFormatSnapshot(ShaderFormatDirectory, id, *snapshot.Value()));
        REQUIRE(snapshots.TryPushBack(Move(snapshot.Value())));
    }

    // 엔진 셰이더 타입의 정점 입력 원문이다(정점 입력 축 타입은 그 축의 첫 정점 입력 타입으로
    // 컴파일).
    const Span<const ShaderVertexInputType> inputs = render::EngineVertexInputTypes();
    REQUIRE(!inputs.IsEmpty());
    Array<ShaderCompileJob> jobs(TestAllocator());
    Array<const char*> names(TestAllocator());
    for (const UniquePtr<ShaderFormatSnapshot>& snapshot : snapshots) {
        for (const TestEntry& test : TestEntries) {
            ShaderCompileJob job;
            job.format = snapshot.Get();
            job.entryPath = StringView(test.path);
            job.entryPoint = StringView(test.entry);
            job.stage = test.stage;
            job.options = DefaultShaderCompileOptions(snapshot->definition);
            REQUIRE(jobs.TryPushBack(job));
            REQUIRE(names.TryPushBack(test.entry));
        }

        for (const ShaderTypeDesc* type : render::EngineShaderTypes()) {
            ShaderCompileJob job;
            job.format = snapshot.Get();
            job.entryPath = type->sourcePath;
            job.entryPoint = type->entryPoint;
            job.stage = type->stage;
            job.options = DefaultShaderCompileOptions(snapshot->definition);
            job.parameterLayouts = type->parameterLayouts;
            job.requiredFeatures = type->requiredFeatures;
            if ((type->flags & ShaderTypeVertexInputAxis) != 0) {
                job.vertexInputSource = inputs[0].sourcePath;
            }

            REQUIRE(jobs.TryPushBack(job));
            REQUIRE(names.TryPushBack(type->name.Data()));
        }
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

    const usize perFormat = jobs.Size() / snapshots.Size();
    for (usize format = 0; format < snapshots.Size(); ++format) {
        const StringView id = Formats[format];
        if (snapshots[format]->definition.stages == 0) {
            // 자리표시 형식(단계 없음 — 후속 플랫폼)이다. job은 InvalidJob으로 끝난다.
            printf(
                "[SKIP] EngineShaders_CompileMatrix %.*s: the format definition is a placeholder "
                "without stages\n",
                static_cast<int>(id.ByteCount()), id.Data());
            continue;
        }

        usize compiled = 0;
        usize skipped = 0;
        const char* skipReason = "";
        for (usize index = format * perFormat; index < (format + 1) * perFormat; ++index) {
            const ShaderCompileJobResult& result = results[index];
            if (result.status == ShaderCompileStatus::Success) {
                ++compiled;
                continue;
            }

            if (IsHostUnsupported(result)) {
                ++skipped;
                skipReason = ShaderCompileStatusName(result.status);
                continue;
            }

            fprintf(stderr, "%.*s %s: %s\n%.*s\n", static_cast<int>(id.ByteCount()), id.Data(),
                    names[index], ShaderCompileStatusName(result.status),
                    static_cast<int>(result.diagnostics.ByteCount()), result.diagnostics.Data());
            CHECK(result.status == ShaderCompileStatus::Success);
        }

        if (skipped == perFormat) {
            printf(
                "[SKIP] EngineShaders_CompileMatrix %.*s: this host cannot build the format (%s)\n",
                static_cast<int>(id.ByteCount()), id.Data(), skipReason);
        } else {
            // 한 형식 안에서 일부만 도구 실패면 원문·도구 문제다(전부 아니면 SKIP이 아니다).
            for (usize index = format * perFormat; skipped != 0 && index < (format + 1) * perFormat;
                 ++index) {
                if (IsHostUnsupported(results[index])) {
                    fprintf(stderr, "%.*s %s: %s\n%.*s\n", static_cast<int>(id.ByteCount()),
                            id.Data(), names[index], ShaderCompileStatusName(results[index].status),
                            static_cast<int>(results[index].diagnostics.ByteCount()),
                            results[index].diagnostics.Data());
                }
            }

            CHECK(skipped == 0);
            printf("  EngineShaders_CompileMatrix %.*s: %zu entries compiled\n",
                   static_cast<int>(id.ByteCount()), id.Data(), compiled);
        }
    }
}

} // namespace engine_shader_compile_matrix_tests_file
