#include "ShaderManagerTestSupport.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Platform/Process.h>
#include <RenderBase/EngineShaderTypes.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCore/ShaderLibraryContents.h>
#include <ShaderCore/ShaderPermutation.h>
#include <ShaderCore/ShaderType.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 엔진 셰이더 SL0·SL1 컴파일 행렬(Docs/Design/EngineShaders 7.6·7.7절 1항, 8.1절 (C), 14.6절 1항,
// 결정 SLD19)이다. 시험 셰이더 폴더 Programs/Sandbox/EngineShaderTests를
// /Plugin/EngineShaderTests로, Engine/Shaders를 /Engine으로 mount하고 실제 ShaderCompileWorker로
// 시험 엔트리(include 폐포 VS·PS·CS와 영역 probe, SL1 compute·유틸리티 probe — 퍼뮤테이션은
// 정의로)와 엔진 셰이더 타입 전부 (RenderBase EngineShaderTypes — 형식 사실로 거른 퍼뮤테이션 전부,
// 쿠킹과 같은 열거 EnumerateShaderLibraryCombinations)를 형식 정의 전부에 컴파일한다. 경고는
// 오류다(워커 -WX·FXC WARNINGS_ARE_ERRORS). 이 호스트가 만들 수 없는 형식(Windows의 Metal —
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

struct TestEntry {
    const char* path;
    const char* entry;
    uint32 stage;
    // 시험 퍼뮤테이션 정의다(그룹 연산 probe의 그룹 크기·경로).
    Span<const ShaderDefine> defines;
};

// 그룹 연산 probe 퍼뮤테이션(PROBE_GROUP_SIZE 0·1·2 × PROBE_FORCE_GROUPSHARED)의 정의다.
constexpr ShaderDefine GroupProbe64[] = {{"PROBE_GROUP_SIZE", "0"},
                                         {"PROBE_FORCE_GROUPSHARED", "0"}};
constexpr ShaderDefine GroupProbe256[] = {{"PROBE_GROUP_SIZE", "1"},
                                          {"PROBE_FORCE_GROUPSHARED", "0"}};
constexpr ShaderDefine GroupProbe100[] = {{"PROBE_GROUP_SIZE", "2"},
                                          {"PROBE_FORCE_GROUPSHARED", "0"}};
constexpr ShaderDefine GroupProbe64Forced[] = {{"PROBE_GROUP_SIZE", "0"},
                                               {"PROBE_FORCE_GROUPSHARED", "1"}};
constexpr ShaderDefine GroupProbe256Forced[] = {{"PROBE_GROUP_SIZE", "1"},
                                                {"PROBE_FORCE_GROUPSHARED", "1"}};
constexpr ShaderDefine GroupProbe100Forced[] = {{"PROBE_GROUP_SIZE", "2"},
                                                {"PROBE_FORCE_GROUPSHARED", "1"}};
constexpr char ComputeProbes[] = "/Plugin/EngineShaderTests/Common/ComputeProbes.rsf";
constexpr char UtilityProbes[] = "/Plugin/EngineShaderTests/Utility/UtilityProbes.rsf";

// 시험 엔트리다(Sandbox EngineShaderTestTypes.inl과 같은 엔트리 — 이 시험은 레이아웃 표 대조를 하지
// 않는다. 레이아웃 대조는 Sandbox 등록 타입이 장치 형식마다 한다).
constexpr TestEntry TestEntries[] = {
    {"/Plugin/EngineShaderTests/Common/IncludeClosure.rsf",
     "IncludeClosureVertex",
     ShaderStageVertex,
     {}},
    {"/Plugin/EngineShaderTests/Common/IncludeClosure.rsf",
     "IncludeClosurePixel",
     ShaderStagePixel,
     {}},
    {"/Plugin/EngineShaderTests/Common/IncludeClosure.rsf",
     "IncludeClosureCompute",
     ShaderStageCompute,
     {}},
    {"/Plugin/EngineShaderTests/Math/MathProbes.rsf", "MathFloatProbePixel", ShaderStagePixel, {}},
    {"/Plugin/EngineShaderTests/Math/MathProbes.rsf", "MathUintProbePixel", ShaderStagePixel, {}},
    {"/Plugin/EngineShaderTests/Color/ColorProbes.rsf",
     "ColorFloatProbePixel",
     ShaderStagePixel,
     {}},
    {"/Plugin/EngineShaderTests/Shading/ShadingTestKernels.rsf",
     "ShadingFloatProbePixel",
     ShaderStagePixel,
     {}},
    {ComputeProbes, "ComputeDispatchProbe", ShaderStageCompute, {}},
    {ComputeProbes, "WrappedDispatchProbe", ShaderStageCompute, {}},
    {ComputeProbes, "WriteDispatchArgumentsProbe", ShaderStageCompute, {}},
    {ComputeProbes, "GroupOperationsProbe", ShaderStageCompute, GroupProbe64},
    {ComputeProbes, "GroupOperationsProbe", ShaderStageCompute, GroupProbe256},
    {ComputeProbes, "GroupOperationsProbe", ShaderStageCompute, GroupProbe100},
    {ComputeProbes, "GroupOperationsProbe", ShaderStageCompute, GroupProbe64Forced},
    {ComputeProbes, "GroupOperationsProbe", ShaderStageCompute, GroupProbe256Forced},
    {ComputeProbes, "GroupOperationsProbe", ShaderStageCompute, GroupProbe100Forced},
    {UtilityProbes, "UtilityProbePixel", ShaderStagePixel, {}},
    {UtilityProbes, "MipPatternPixel", ShaderStagePixel, {}}};

// 퍼뮤테이션 정의 문자열 한 벌이다(job이 가리키므로 컴파일이 끝날 때까지 산다).
constexpr usize DefineValueBytes = 12;

struct PermutationDefines {
    ShaderDefine defines[MaxShaderPermutationDimensions] = {};
    char values[MaxShaderPermutationDimensions][DefineValueBytes] = {};
    usize count = 0;
};

// 조합의 퍼뮤테이션 정의를 out에 채운다(차원 선언 순서 — 쿠킹 FillDefines와 같은 값).
bool FillPermutationDefines(const ShaderLibraryCombination& combination,
                            PermutationDefines& out) noexcept {
    const ShaderPermutationDomain& domain = combination.type->permutation;
    const usize dimensions = domain.dimensions.Size();
    uint32 valueIds[MaxShaderPermutationDimensions] = {};
    if (!DecodeShaderPermutation(domain, combination.permutationId,
                                 Span<uint32>(valueIds, dimensions))) {
        return false;
    }

    for (usize index = 0; index < dimensions; ++index) {
        const Result<int32> value =
            ShaderPermutationDefineValue(domain.dimensions[index], valueIds[index]);
        if (!value) {
            return false;
        }

        (void)snprintf(out.values[index], DefineValueBytes, "%d", static_cast<int>(value.Value()));
        out.defines[index] =
            ShaderDefine{domain.dimensions[index].defineName, StringView(out.values[index])};
    }

    out.count = dimensions;
    return true;
}

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
    for (const StringView id : ShaderFormatIds) {
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
    Array<uint32> permutations(TestAllocator());
    // 형식마다 job 구간 [begin, end)다(형식 사실 거르기로 형식마다 조합 수가 다르다).
    Array<usize> formatBegins(TestAllocator());
    // 퍼뮤테이션 정의 저장소다(Array가 자라도 job이 가리키는 정의가 옮겨지지 않게 한 벌씩 따로
    // 둔다).
    Array<UniquePtr<PermutationDefines>> defineStorage(TestAllocator());
    for (const UniquePtr<ShaderFormatSnapshot>& snapshot : snapshots) {
        REQUIRE(formatBegins.TryPushBack(jobs.Size()));
        for (const TestEntry& test : TestEntries) {
            ShaderCompileJob job;
            job.format = snapshot.Get();
            job.entryPath = StringView(test.path);
            job.entryPoint = StringView(test.entry);
            job.stage = test.stage;
            job.defines = test.defines;
            job.options = DefaultShaderCompileOptions(snapshot->definition);
            REQUIRE(jobs.TryPushBack(job));
            REQUIRE(names.TryPushBack(test.entry));
            REQUIRE(permutations.TryPushBack(0u));
        }

        // 엔진 셰이더 타입의 조합(쿠킹과 같은 열거 — 형식 사실로 거른 퍼뮤테이션 전부)이다. 정점
        // 입력 축 타입은 그 축의 정점 입력 타입마다 하나다.
        Array<ShaderLibraryCombination> combinations(TestAllocator());
        REQUIRE(EnumerateShaderLibraryCombinations(render::EngineShaderTypes(), inputs,
                                                   MakeShaderFormatFacts(snapshot->definition),
                                                   ShaderLibraryKind::Engine, false, combinations));
        for (const ShaderLibraryCombination& combination : combinations) {
            const ShaderTypeDesc* type = combination.type;
            auto defines = UniquePtr<PermutationDefines>::TryMake(TestAllocator());
            REQUIRE(defines);
            REQUIRE(FillPermutationDefines(combination, *defines.Value()));
            ShaderCompileJob job;
            job.format = snapshot.Get();
            job.entryPath = type->sourcePath;
            job.entryPoint = type->entryPoint;
            job.stage = type->stage;
            job.defines =
                Span<const ShaderDefine>(defines.Value()->defines, defines.Value()->count);
            job.options = DefaultShaderCompileOptions(snapshot->definition);
            job.parameterLayouts = type->parameterLayouts;
            job.requiredFeatures = type->requiredFeatures;
            for (const ShaderVertexInputType& input : inputs) {
                if (input.id == combination.vertexInputType) {
                    job.vertexInputSource = input.sourcePath;
                }
            }

            REQUIRE(defineStorage.TryPushBack(Move(defines.Value())));
            REQUIRE(jobs.TryPushBack(job));
            REQUIRE(names.TryPushBack(type->name.Data()));
            REQUIRE(permutations.TryPushBack(combination.permutationId));
        }
    }

    REQUIRE(formatBegins.TryPushBack(jobs.Size()));

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
        const usize begin = formatBegins[format];
        const usize end = formatBegins[format + 1];
        const usize perFormat = end - begin;
        const StringView id = ShaderFormatIds[format];
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
        for (usize index = begin; index < end; ++index) {
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

            fprintf(stderr, "%.*s %s (permutation %u): %s\n%.*s\n",
                    static_cast<int>(id.ByteCount()), id.Data(), names[index], permutations[index],
                    ShaderCompileStatusName(result.status),
                    static_cast<int>(result.diagnostics.ByteCount()), result.diagnostics.Data());
            CHECK(result.status == ShaderCompileStatus::Success);
        }

        if (skipped == perFormat) {
            printf(
                "[SKIP] EngineShaders_CompileMatrix %.*s: this host cannot build the format (%s)\n",
                static_cast<int>(id.ByteCount()), id.Data(), skipReason);
        } else {
            // 한 형식 안에서 일부만 도구 실패면 원문·도구 문제다(전부 아니면 SKIP이 아니다).
            for (usize index = begin; skipped != 0 && index < end; ++index) {
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
