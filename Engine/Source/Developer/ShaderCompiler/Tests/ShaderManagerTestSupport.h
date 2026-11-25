#pragma once

#include "ShaderSourceTestSupport.h"

#include <Core/Text/String.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderWorkerChannel.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 셰이더 컴파일 관리자 시험 공용 준비다(실제 워커·도구 저장소·시험 원문·형식 snapshot). 관리자
// 시험과 바인딩 규칙 시험(Docs/Design/ShaderSystem 16.1절 SSD29 SH2b1)이 함께 쓴다. 워커가 없거나
// 도구 저장소가 준비되지 않았으면 Make가 SKIP 이유를 돌려준다.
namespace rocks::shader_manager_test {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

// bool을 돌려주는 준비 함수의 REQUIRE다(실패를 기록하고 false).
#define SHADER_MANAGER_TEST_REQUIRE(expression)                                                    \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::rocks::test::RecordFailure(#expression, __FILE__, __LINE__);                         \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

inline constexpr usize ManagerPathBytes = 4096;
// 확실히 살아 있지 않은 PID다(Windows PID는 4의 배수, POSIX pid_max보다 크다).
inline constexpr char DeadPidName[] = "4294967294";
// 시험 레지스트리 문맥 id다("SHMT").
inline constexpr uint32 TestContextId = 0x53484d54;

inline constexpr char TestSource[] =
    "RWStructuredBuffer<uint> Output;\n"
    "[numthreads(4, 1, 1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID) { Output[id.x] = id.x * SCALE; }\n"
    "float4 PSMain() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";
inline constexpr char TestEntry[] = "/Project/Test.rsf";
inline constexpr char NuggetSettings[] =
    "{ \"localPath\": \"Cache/Shader\", \"required\": true, "
    "\"generation\": \"01234567-89ab-cdef-0123-456789abcdef\" }";

// 시험 실행 파일 옆의 워커다.
inline bool WorkerExecutable(char (&out)[ManagerPathBytes]) noexcept {
    usize bytes = 0;
    if (!platform::GetExecutablePath(out, sizeof(out), bytes)) {
        return false;
    }

    while (bytes > 0 && out[bytes - 1] != '/' && out[bytes - 1] != '\\') {
        --bytes;
    }

    for (const char* name : {"ShaderCompileWorker.exe", "ShaderCompileWorker"}) {
        const int written = snprintf(out + bytes, sizeof(out) - bytes, "%s", name);
        bool exists = false;
        if (written > 0 && platform::PhysicalFileSystem().FileExists(out, strlen(out), exists) &&
            exists) {
            return true;
        }
    }

    return false;
}

// 도구 저장소가 준비되었는가(워커 도구 조회).
inline bool ToolsReady(const char* worker, const Fixture& fixture) noexcept {
    char directory[ManagerPathBytes];
    if (!fixture.Child("ToolQuery", directory, sizeof(directory))) {
        return false;
    }

    ShaderWorkerChannel channel(TestAllocator());
    ShaderWorkerLaunch launch;
    launch.executable = StringView(worker);
    launch.workDirectory = StringView(directory);
    if (!channel.Start(launch)) {
        return false;
    }

    ShaderWorkerRequest request(TestAllocator());
    request.kind = ShaderWorkerRequestKind::ToolQuery;
    Array<uint8> bytes(TestAllocator());
    ShaderWorkerBatchResult result(TestAllocator());
    if (!MakeShaderWorkerHeader({}, request.header) || !EncodeShaderWorkerRequest(request, bytes)) {
        return false;
    }

    channel.Run(bytes.View(), result);
    bool ready =
        result.outcome == ShaderWorkerBatchOutcome::Completed && !result.response.tools.IsEmpty();
    for (const ShaderWorkerToolCheck& tool : result.response.tools) {
        ready = ready && tool.state == ShaderWorkerToolState::Ready;
    }

    return ready;
}

// 리플렉션 문맥이다(옵션 기록).
struct KeyEnv {
    KeyEnv() noexcept
        : types(TestAllocator()), registration(types, TestAllocator()),
          objects(TestAllocator(), types, {TestContextId, 1}), access(objects.OwnerAccess()) {
        registered = static_cast<bool>(RegisterShaderCompilerTypes(registration));
    }

    ~KeyEnv() noexcept {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    ShaderKeyReflection Reflection() noexcept {
        return ShaderKeyReflection{&objects, access, &types, &TestAllocator()};
    }

    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    bool registered = false;
};

// 워커·도구·원문·형식이 준비된 시험 한 벌이다.
struct ManagerCase {
    ManagerCase() noexcept
        : tree(TestAllocator()), d3d12(TestAllocator()), vulkan(TestAllocator()) {
    }

    // false면 SKIP(이유는 skip에)이거나 준비 실패(skip은 null — 실패를 기록했다)다.
    bool Make(const char* tag, const char*& skip) noexcept {
        if (!WorkerExecutable(worker)) {
            skip = "ShaderCompileWorker is not next to the test executable";
            return false;
        }

        char engine[ManagerPathBytes];
        char project[ManagerPathBytes];
        SHADER_MANAGER_TEST_REQUIRE(fixture.Make(tag));
        SHADER_MANAGER_TEST_REQUIRE(fixture.Write("Project/Shaders/Test.rsf", TestSource));
        SHADER_MANAGER_TEST_REQUIRE(fixture.Child("Project", project, sizeof(project)));
        SHADER_MANAGER_TEST_REQUIRE(RepositoryShaderDirectory(engine, sizeof(engine)));
        SHADER_MANAGER_TEST_REQUIRE(tree.Mount("/Engine", StringView(engine)));
        char shaders[ManagerPathBytes];
        snprintf(shaders, sizeof(shaders), "%s/Shaders", project);
        SHADER_MANAGER_TEST_REQUIRE(tree.Mount("/Project", StringView(shaders)));
        SHADER_MANAGER_TEST_REQUIRE(
            LoadShaderFormatSnapshot(ShaderFormatDirectory, "D3D12_SM6", d3d12));
        SHADER_MANAGER_TEST_REQUIRE(
            LoadShaderFormatSnapshot(ShaderFormatDirectory, "VULKAN_SM5", vulkan));
        SHADER_MANAGER_TEST_REQUIRE(env.registered);
        if (!ToolsReady(worker, fixture)) {
            skip = "shader tool store is not bootstrapped (dotnet msbuild Rocks.proj "
                   "/t:ThirdPartyBootstrap)";
            return false;
        }

        return true;
    }

    // 작업 디렉터리 뿌리 하나다(시험마다 다르게 준다).
    ShaderCompileManagerConfig Config(const char* workRoot) noexcept {
        snprintf(work, sizeof(work), "%s/%s", fixture.Path(), workRoot);
        ShaderCompileManagerConfig config;
        config.allocator = &TestAllocator();
        config.tree = &tree;
        config.reflection = env.Reflection();
        config.workerExecutable = StringView(worker);
        config.workRoot = StringView(work);
        return config;
    }

    // 기본 job 목록: 두 형식 × (compute 두 정의 + pixel).
    void Jobs(Array<ShaderCompileJob>& jobs) noexcept {
        for (const ShaderFormatSnapshot* format : {&d3d12, &vulkan}) {
            for (usize variant = 0; variant < 3; ++variant) {
                ShaderCompileJob job;
                job.format = format;
                job.entryPath = TestEntry;
                job.entryPoint = variant == 2 ? "PSMain" : "CSMain";
                job.stage = variant == 2 ? ShaderStagePixel : ShaderStageCompute;
                job.defines = Span<const ShaderDefine>(&scales[variant], 1);
                job.options = DefaultShaderCompileOptions(format->definition);
                REQUIRE(jobs.TryPushBack(job));
            }
        }
    }

    Fixture fixture;
    ShaderSourceTree tree;
    ShaderFormatSnapshot d3d12;
    ShaderFormatSnapshot vulkan;
    KeyEnv env;
    char worker[ManagerPathBytes] = {};
    char work[ManagerPathBytes] = {};
    const ShaderDefine scales[3] = {{"SCALE", "2"}, {"SCALE", "3"}, {"SCALE", "1"}};
};

// 성공해야 하는 앞 count개 job의 실패를 찍는다.
inline void PrintFailures(const Array<ShaderCompileJobResult>& results, usize count) noexcept {
    for (usize index = 0; index < count && index < results.Size(); ++index) {
        if (results[index].status != ShaderCompileStatus::Success) {
            fprintf(stderr, "job %zu %s: %.*s\n", index,
                    ShaderCompileStatusName(results[index].status),
                    static_cast<int>(results[index].diagnostics.ByteCount()),
                    results[index].diagnostics.Data());
        }
    }
}

} // namespace rocks::shader_manager_test
