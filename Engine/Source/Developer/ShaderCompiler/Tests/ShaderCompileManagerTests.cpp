#include "ShaderManagerTestSupport.h"

#include <AssetNugget/NuggetCache.h>
#include <Base/NumericLimits.h>
#include <Core/FileRead.h>
#include <Core/Text/String.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompiler/ShaderCompileCommandlet.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderLibraryCook.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCompiler/ShaderWorkerChannel.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <Task/WorkerScheduler.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 셰이더 컴파일 관리자와 shader-compile 명령 시험이다(Docs/Design/ShaderSystem 10절, 11.1절
// SC4·SC11–SC13, SH1d): 너겟 조회 결과 분류, 설정 검사, 다른 PID 작업 디렉터리 정리, 같은 입력의
// 같은 byte(워커 수·묶음 크기·작업 디렉터리와 무관), 찬 캐시(워커 0개), 값 0 없는 Record의
// 재컴파일, 실패 묶음 격리, 진행 계수 정산, 명령 출력·종료 코드·출력 파일 이름, 라이브러리
// 쿠킹(SH2a2 — 거르기·정점 입력 축·개발 전용·코드 중복 제거·파일 이름·다시 쿠킹 Unchanged·같은 이름
// 다른 byte CorruptEntry·job 실패 시 쓰지 않음). 워커가 없거나 도구 저장소가 준비되지 않았으면 그
// 시험만 SKIP이다.
using namespace rocks;

namespace shader_compile_manager_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;
using namespace rocks::shader_manager_test;

namespace {

bool SameCode(const ShaderCompileJobResult& left, const ShaderCompileJobResult& right) noexcept {
    if (!left.values.Has(ShaderNuggetValue::Code) || !right.values.Has(ShaderNuggetValue::Code)) {
        return false;
    }

    const Array<uint8>& a = left.values.Part(ShaderNuggetValue::Code);
    const Array<uint8>& b = right.values.Part(ShaderNuggetValue::Code);
    return a.Size() == b.Size() && a.Size() > 0 && memcmp(a.Data(), b.Data(), a.Size()) == 0;
}

// 명령 출력을 모은다.
struct Captured {
    Captured() noexcept : out(TestAllocator()), error(TestAllocator()) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        auto& self = *static_cast<Captured*>(context);
        return (stream == CommandletStream::Output ? self.out : self.error).TryAppend(text);
    }

    CommandletOutput Output() noexcept {
        CommandletOutput output;
        output.write = &Write;
        output.context = this;
        return output;
    }

    void Clear() noexcept {
        out.Clear();
        error.Clear();
    }

    String out;
    String error;
};

// 명령을 돌려 종료 코드가 expected인지 본다(다를 때만 출력을 찍는다 — 기대한 실패의 진단은 찍지
// 않는다).
bool RunCommand(Captured& captured, ShaderCompileCommandContext& context, int32 expected,
                std::initializer_list<StringView> args) noexcept {
    captured.Clear();
    CommandletOutput output = captured.Output();
    auto code = RunShaderCompileCommand(&context, Span<const StringView>(args.begin(), args.size()),
                                        output);
    if (!code) {
        CHECK(code);
        return false;
    }

    if (code.Value() != expected) {
        fprintf(stderr, "shader-compile exit %d (expected %d)\nstdout:\n%s\nstderr:\n%s\n",
                code.Value(), expected, captured.out.Data(), captured.error.Data());
    }

    return code.Value() == expected;
}

bool Contains(const String& text, const char* part) noexcept {
    return strstr(text.Data(), part) != nullptr;
}

} // namespace

TEST(ShaderCompileManager_ClassifiesNuggetResults) {
    NuggetCacheGetResult hit(TestAllocator());
    hit.status = NuggetStatus::Hit;
    auto code = hit.values.TryEmplaceBack(TestAllocator());
    REQUIRE(code);
    code.Value()->id = static_cast<uint32>(ShaderNuggetValue::Code);
    CHECK(ClassifyShaderNuggetGet(hit, false) == ShaderNuggetAction::UseHit);
    CHECK(ClassifyShaderNuggetGet(hit, true) == ShaderNuggetAction::UseHit);

    // 값 0이 없는 적중은 손상이라 다시 컴파일한다.
    NuggetCacheGetResult layoutOnly(TestAllocator());
    layoutOnly.status = NuggetStatus::Hit;
    auto layout = layoutOnly.values.TryEmplaceBack(TestAllocator());
    REQUIRE(layout);
    layout.Value()->id = static_cast<uint32>(ShaderNuggetValue::BindingLayout);
    CHECK(ClassifyShaderNuggetGet(layoutOnly, true) == ShaderNuggetAction::Recompile);

    for (const NuggetStatus status :
         {NuggetStatus::Miss, NuggetStatus::Corrupt, NuggetStatus::DigestChanged,
          NuggetStatus::Dropped, NuggetStatus::Partial, NuggetStatus::Unknown}) {
        NuggetCacheGetResult result(TestAllocator());
        result.status = status;
        CHECK(ClassifyShaderNuggetGet(result, false) == ShaderNuggetAction::Recompile);
        CHECK(ClassifyShaderNuggetGet(result, true) == ShaderNuggetAction::Recompile);
    }

    NuggetCacheGetResult cancelled(TestAllocator());
    cancelled.status = NuggetStatus::Cancelled;
    CHECK(ClassifyShaderNuggetGet(cancelled, true) == ShaderNuggetAction::Cancel);

    // 캐시 실패: required면 job 오류, 아니면 다시 컴파일.
    NuggetCacheGetResult failed(TestAllocator());
    failed.status = NuggetStatus::Miss;
    failed.failed = true;
    CHECK(ClassifyShaderNuggetGet(failed, true) == ShaderNuggetAction::Fail);
    CHECK(ClassifyShaderNuggetGet(failed, false) == ShaderNuggetAction::Recompile);
    CHECK(StringView(ShaderCompileStatusName(ShaderCompileStatus::WorkerFailed)) == "WorkerFailed");
}

TEST(ShaderCompileManager_RejectsIncompleteConfig) {
    ShaderCompileManagerConfig config;
    ShaderCompileManager empty(config);
    CHECK(empty.Start().Error() == ErrorCode::InvalidArgument);
    Array<ShaderCompileJobResult> results(TestAllocator());
    CHECK(empty.Compile(Span<const ShaderCompileJob>(), results).Error() == ErrorCode::StaleHandle);
    ShaderCompileJobResult one(TestAllocator());
    CHECK(empty.CompileNow(ShaderCompileJob{}, one).Error() == ErrorCode::StaleHandle);
    CHECK(empty.Progress().total == 0);
    empty.Shutdown();
}

TEST(ShaderCompileManager_CleansStaleWorkDirectories) {
    Fixture fixture;
    REQUIRE(fixture.Make("managerclean"));
    char own[32];
    snprintf(own, sizeof(own), "Work/%u/old.txt", platform::CurrentProcessId());
    char dead[64];
    snprintf(dead, sizeof(dead), "Work/%s/0/input.1", DeadPidName);
    REQUIRE(fixture.Write(own, "own"));
    REQUIRE(fixture.Write(dead, "dead"));
    REQUIRE(fixture.Write("Work/notpid/keep.txt", "keep"));
    REQUIRE(fixture.Write("Work/12345678901/keep.txt", "too long for a pid"));

    ShaderSourceTree tree(TestAllocator());
    KeyEnv env;
    char work[ManagerPathBytes];
    REQUIRE(fixture.Child("Work", work, sizeof(work)));
    ShaderCompileManagerConfig config;
    config.allocator = &TestAllocator();
    config.tree = &tree;
    config.reflection = env.Reflection();
    // 워커는 첫 묶음에서 띄우므로 Start에는 실행 파일이 없어도 된다.
    config.workerExecutable = "missing-worker";
    config.workRoot = StringView(work);
    config.workerCount = 2;
    ShaderCompileManager manager(config);
    REQUIRE(manager.Start());

    const auto exists = [&](const char* relative, bool directory) noexcept {
        char path[ManagerPathBytes];
        bool found = false;
        SHADER_MANAGER_TEST_REQUIRE(fixture.Child(relative, path, sizeof(path)));
        platform::FileSystem& files = platform::PhysicalFileSystem();
        SHADER_MANAGER_TEST_REQUIRE(directory ? files.DirectoryExists(path, strlen(path), found)
                                              : files.FileExists(path, strlen(path), found));
        return found;
    };
    char deadDirectory[32];
    snprintf(deadDirectory, sizeof(deadDirectory), "Work/%s", DeadPidName);
    CHECK(!exists(deadDirectory, true));
    CHECK(!exists(own, false));
    CHECK(exists("Work/notpid/keep.txt", false));
    CHECK(exists("Work/12345678901/keep.txt", false));
    CHECK(manager.Progress().workersStarted == 0);
    manager.Shutdown();
    // 두 번째 Start는 막는다(이미 시작함은 Shutdown 뒤 다시 된다).
    REQUIRE(manager.Start());
    CHECK(manager.Start().Error() == ErrorCode::AlreadyExists);
}

TEST(ShaderCompileManager_CompilesDeterministically) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("managerdeterministic", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileManager_CompilesDeterministically: %s\n", skip);
        }

        return;
    }

    Array<ShaderCompileJob> jobs(TestAllocator());
    test.Jobs(jobs);
    // 잘못된 job·전처리 오류도 섞는다(다른 job에 영향이 없다).
    ShaderCompileJob missing = jobs[0];
    missing.entryPath = "/Project/Missing.rsf";
    REQUIRE(jobs.TryPushBack(missing));
    ShaderCompileJob reserved = jobs[0];
    const ShaderDefine reservedDefine{"ROCKS_BACKEND", "1"};
    reserved.defines = Span<const ShaderDefine>(&reservedDefine, 1);
    REQUIRE(jobs.TryPushBack(reserved));

    Array<ShaderCompileJobResult> first(TestAllocator());
    ShaderCompileProgress firstProgress;
    {
        ShaderCompileManagerConfig config = test.Config("WorkA");
        config.workerCount = 1;
        ShaderCompileManager manager(config);
        REQUIRE(manager.Start());
        REQUIRE(manager.Compile(jobs.View(), first));
        firstProgress = manager.Progress();
    }

    REQUIRE(first.Size() == jobs.Size());
    PrintFailures(first, 6);
    for (usize index = 0; index < 6; ++index) {
        CHECK(first[index].status == ShaderCompileStatus::Success && first[index].hasKey &&
              !first[index].cacheHit);
    }

    CHECK(first[6].status == ShaderCompileStatus::PreprocessError &&
          Contains(first[6].diagnostics, "/Project/Missing.rsf"));
    CHECK(first[7].status == ShaderCompileStatus::InvalidJob && !first[7].hasKey);
    // 정의가 다르면 키와 byte가 다르다.
    CHECK(!(first[0].key == first[1].key));
    CHECK(!SameCode(first[0], first[1]));
    CHECK(firstProgress.total == 8 && firstProgress.completed == 6 && firstProgress.failed == 2 &&
          firstProgress.cacheHits == 0 && firstProgress.provisional == 0 &&
          firstProgress.batches == 1 && firstProgress.workersStarted == 1);

    // 워커 수·묶음 크기·작업 디렉터리를 바꿔도 같은 byte다.
    Array<ShaderCompileJobResult> second(TestAllocator());
    ShaderCompileProgress secondProgress;
    {
        ShaderCompileManagerConfig config = test.Config("WorkB");
        config.workerCount = 3;
        config.maxBatchJobs = 1;
        ShaderCompileManager manager(config);
        REQUIRE(manager.Start());
        REQUIRE(manager.Compile(jobs.View(), second));
        // 두 번째 Compile은 계수에 더한다.
        Array<ShaderCompileJobResult> again(TestAllocator());
        REQUIRE(manager.Compile(Span<const ShaderCompileJob>(jobs.Data(), 1), again));
        CHECK(again[0].status == ShaderCompileStatus::Success && SameCode(again[0], first[0]));
        secondProgress = manager.Progress();
    }

    for (usize index = 0; index < 6; ++index) {
        CHECK(second[index].key == first[index].key);
        CHECK(SameCode(second[index], first[index]));
    }

    CHECK(secondProgress.total == 9 && secondProgress.completed == 7 &&
          secondProgress.failed == 2 && secondProgress.provisional == 0 &&
          secondProgress.batches == 7 && secondProgress.workersStarted == 3);
}

TEST(ShaderCompileManager_UsesWarmNuggetCache) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("managercache", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileManager_UsesWarmNuggetCache: %s\n", skip);
        }

        return;
    }

    REQUIRE(test.fixture.Write("Project/Config/AssetNuggets.json", NuggetSettings));
    char project[ManagerPathBytes];
    REQUIRE(test.fixture.Child("Project", project, sizeof(project)));
    Array<ShaderCompileJob> jobs(TestAllocator());
    test.Jobs(jobs);
    NuggetSystemClock clock;
    WorkerScheduler scheduler(TestAllocator());
    REQUIRE(scheduler.Start(WorkerSchedulerConfig{2}));

    Array<ShaderCompileJobResult> cold(TestAllocator());
    Array<ShaderCompileJobResult> warm(TestAllocator());
    for (const bool second : {false, true}) {
        NuggetCache cache(TestAllocator(), platform::PhysicalFileSystem(), clock);
        REQUIRE(OpenShaderNuggetCache(cache, TestAllocator(), platform::PhysicalFileSystem(),
                                      StringView(project), nullptr));
        ShaderCompileManagerConfig config = test.Config("Work");
        config.cache = &cache;
        config.scheduler = &scheduler;
        config.workerCount = 2;
        ShaderCompileManager manager(config);
        REQUIRE(manager.Start());
        Array<ShaderCompileJobResult>& results = second ? warm : cold;
        REQUIRE(manager.Compile(jobs.View(), results));
        PrintFailures(results, results.Size());
        const ShaderCompileProgress progress = manager.Progress();
        for (usize index = 0; index < jobs.Size(); ++index) {
            CHECK(results[index].status == ShaderCompileStatus::Success &&
                  results[index].cacheHit == second);
        }

        if (second) {
            // 찬 캐시는 워커를 띄우지 않는다.
            CHECK(progress.cacheHits == jobs.Size() && progress.workersStarted == 0 &&
                  progress.batches == 0 && progress.completed == jobs.Size());
            // CompileNow(동기 Get·Put): 적중과 새 job.
            ShaderCompileJobResult now(TestAllocator());
            REQUIRE(manager.CompileNow(jobs[0], now));
            CHECK(now.status == ShaderCompileStatus::Success && now.cacheHit &&
                  SameCode(now, cold[0]));
            ShaderCompileJob fresh = jobs[0];
            const ShaderDefine scale{"SCALE", "7"};
            fresh.defines = Span<const ShaderDefine>(&scale, 1);
            REQUIRE(manager.CompileNow(fresh, now));
            CHECK(now.status == ShaderCompileStatus::Success && !now.cacheHit);
            CHECK(manager.Progress().workersStarted == 1);
        } else {
            CHECK(progress.cacheHits == 0 && progress.conflicts == 0 &&
                  progress.completed == jobs.Size());
        }

        manager.Shutdown();
        cache.Shutdown(0);
    }

    for (usize index = 0; index < jobs.Size(); ++index) {
        CHECK(warm[index].key == cold[index].key && SameCode(warm[index], cold[index]));
    }

    // 값 0이 없는 Record는 손상이라 다시 컴파일한다(같은 세대의 다른 캐시 root에 같은 키로 둔다).
    REQUIRE(test.fixture.Write("Other/Config/AssetNuggets.json", NuggetSettings));
    char other[ManagerPathBytes];
    REQUIRE(test.fixture.Child("Other", other, sizeof(other)));
    NuggetCache cache(TestAllocator(), platform::PhysicalFileSystem(), clock);
    REQUIRE(OpenShaderNuggetCache(cache, TestAllocator(), platform::PhysicalFileSystem(),
                                  StringView(other), nullptr));
    Array<NuggetValue> layoutOnly(TestAllocator());
    auto layout = layoutOnly.TryEmplaceBack(TestAllocator());
    REQUIRE(layout);
    layout.Value()->id = static_cast<uint32>(ShaderNuggetValue::BindingLayout);
    REQUIRE(layout.Value()->bytes.TryPushBack(uint8{1}));
    NuggetCachePut put;
    put.bucket = NuggetBucketShader;
    put.key = cold[0].key;
    put.values = layoutOnly.View();
    CHECK(cache.Put(put).status == NuggetStatus::Stored);
    {
        ShaderCompileManagerConfig config = test.Config("WorkOther");
        config.cache = &cache;
        config.workerCount = 1;
        ShaderCompileManager manager(config);
        REQUIRE(manager.Start());
        ShaderCompileJobResult result(TestAllocator());
        REQUIRE(manager.CompileNow(jobs[0], result));
        CHECK(result.status == ShaderCompileStatus::Success && !result.cacheHit &&
              SameCode(result, cold[0]));
        const ShaderCompileProgress progress = manager.Progress();
        // 같은 키에 다른 값을 게시하려 해서 Conflict(결정성 위반 통계)다.
        CHECK(progress.corruptHits == 1 && progress.conflicts == 1);
        CHECK(Contains(result.diagnostics, "nugget conflict"));
    }

    cache.Shutdown(0);
    REQUIRE(scheduler.Shutdown());
}

TEST(ShaderCompileManager_IsolatesFailingBatch) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("managerisolate", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileManager_IsolatesFailingBatch: %s\n", skip);
        }

        return;
    }

    Array<ShaderCompileJob> jobs(TestAllocator());
    test.Jobs(jobs);
    jobs[1].testHook = ShaderWorkerJobKind::Crash;
    ShaderCompileManagerConfig config = test.Config("Work");
    config.workerCount = 1;
    config.maxBatchJobs = 1;
    config.testHooks = true;
    ShaderCompileManager manager(config);
    REQUIRE(manager.Start());
    Array<ShaderCompileJobResult> results(TestAllocator());
    REQUIRE(manager.Compile(jobs.View(), results));
    for (usize index = 0; index < jobs.Size(); ++index) {
        if (index == 1) {
            CHECK(results[index].status == ShaderCompileStatus::WorkerFailed &&
                  !results[index].diagnostics.IsEmpty() &&
                  !results[index].values.Has(ShaderNuggetValue::Code));
        } else {
            CHECK(results[index].status == ShaderCompileStatus::Success);
        }
    }

    // 충돌한 워커를 다음 묶음에서 다시 띄웠다.
    const ShaderCompileProgress progress = manager.Progress();
    CHECK(progress.workersStarted == 2 && progress.failed == 1 &&
          progress.completed == jobs.Size() - 1 && progress.provisional == 0);

    // hook job은 hook 워커에만 간다.
    ShaderCompileManagerConfig plain = test.Config("WorkPlain");
    ShaderCompileManager refusing(plain);
    REQUIRE(refusing.Start());
    ShaderCompileJobResult refused(TestAllocator());
    REQUIRE(refusing.CompileNow(jobs[1], refused));
    CHECK(refused.status == ShaderCompileStatus::InvalidJob);
}

TEST(ShaderCompileCommand_ReportsJobsAndWritesOutputs) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("compilecommand", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileCommand_ReportsJobsAndWritesOutputs: %s\n", skip);
        }

        return;
    }

    char project[ManagerPathBytes];
    char projectOption[ManagerPathBytes + 16];
    char outOption[ManagerPathBytes + 16];
    REQUIRE(test.fixture.Child("Project", project, sizeof(project)));
    snprintf(projectOption, sizeof(projectOption), "--project=%s", project);
    snprintf(outOption, sizeof(outOption), "--out=%s/Out", test.fixture.Path());
    ShaderCompileCommandContext context;
    context.allocator = &TestAllocator();
    context.workerExecutable = StringView(test.worker);
    Captured captured;

    // 사용법 오류는 2다.
    CHECK(RunCommand(captured, context, 2, {}));
    CHECK(Contains(captured.error, "usage: shader-compile"));
    CHECK(RunCommand(captured, context, 2, {"/Project/Test.rsf:CSMain"}));
    CHECK(RunCommand(captured, context, 2, {"/Project/Test.rsf:CSMain:geometry"}));
    CHECK(RunCommand(captured, context, 2, {"--bogus=1", "/Project/Test.rsf:CSMain:compute"}));
    CHECK(RunCommand(captured, context, 2, {"--workers=0", "/Project/Test.rsf:CSMain:compute"}));
    CHECK(RunCommand(captured, context, 0, {"--help"}));
    CHECK(Contains(captured.out, "usage: shader-compile"));

    const StringView compute = "/Project/Test.rsf:CSMain:compute";
    const StringView pixel = "/Project/Test.rsf:PSMain:pixel";
    CHECK(RunCommand(captured, context, 0,
                     {StringView(projectOption), "--format=D3D12_SM6", "--format=VULKAN_SM5",
                      "--define=SCALE=2", StringView(outOption), compute, pixel}));
    CHECK(Contains(captured.out, "shader-compile: D3D12_SM6 /Project/Test.rsf:CSMain:compute "));
    CHECK(Contains(captured.out, " miss Success "));
    CHECK(Contains(captured.out,
                   "shader-compile: jobs 4 completed 4 failed 0 cache-hits 0 workers "));
    const char* const files[] = {"Out/D3D12_SM6/+Project+Test.rsf.CSMain.compute.dxil",
                                 "Out/D3D12_SM6/+Project+Test.rsf.PSMain.pixel.dxil",
                                 "Out/VULKAN_SM5/+Project+Test.rsf.CSMain.compute.spv",
                                 "Out/VULKAN_SM5/+Project+Test.rsf.PSMain.pixel.spv"};
    for (const char* relative : files) {
        char path[ManagerPathBytes];
        bool exists = false;
        REQUIRE(test.fixture.Child(relative, path, sizeof(path)));
        REQUIRE(platform::PhysicalFileSystem().FileExists(path, strlen(path), exists));
        CHECK(exists);
    }

    // 두 번째는 너겟 적중이고 워커를 띄우지 않는다(작업 디렉터리는 project 아래다).
    CHECK(
        RunCommand(captured, context, 0,
                   {StringView(projectOption), "--format=D3D12_SM6", "--define=SCALE=2", compute}));
    CHECK(Contains(captured.out, " hit Success"));
    CHECK(Contains(captured.out, "cache-hits 1 workers 0 batches 0"));
    char workRoot[ManagerPathBytes];
    bool exists = false;
    REQUIRE(test.fixture.Child("Project/Intermediate/ShaderWorkers", workRoot, sizeof(workRoot)));
    REQUIRE(platform::PhysicalFileSystem().DirectoryExists(workRoot, strlen(workRoot), exists));
    CHECK(exists);

    // job 실패는 1이고 진단은 표준 오류다(캐시 없이).
    CHECK(RunCommand(captured, context, 1,
                     {StringView(projectOption), "--format=D3D12_SM6", "--no-cache",
                      "--define=SCALE=2", "/Project/Test.rsf:NoSuchEntry:compute"}));
    CHECK(Contains(captured.out, " miss CompileError"));
    CHECK(Contains(captured.error, "shader-compile: D3D12_SM6 /Project/Test.rsf:NoSuchEntry"));
    CHECK(RunCommand(captured, context, 1,
                     {StringView(projectOption), "--format=NO_SUCH_FORMAT", compute}));
    CHECK(
        RunCommand(captured, context, 2,
                   {StringView(projectOption), "--format=D3D12_SM6", "--option=bogus=1", compute}));
}

namespace {

// 라이브러리 쿠킹 시험 원문이다(SCALE이 없는 퍼뮤테이션도 컴파일되게 기본값을 둔다).
constexpr char CookSource[] =
    "#ifndef SCALE\n"
    "#define SCALE 1\n"
    "#endif\n"
    "RWStructuredBuffer<uint> Output;\n"
    "[numthreads(4, 1, 1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID) { Output[id.x] = id.x * SCALE; }\n"
    "float4 PSMain() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";
constexpr char CookEntry[] = "/Project/Cook.rsf";

// 라이브러리 쿠킹 시험 타입이다(CookEngine 밖은 Project 소속 — /Project/Cook.rsf).
ROCKS_SHADER_TYPE_NAME(CookComputeName, "CookCompute");
ROCKS_SHADER_TYPE_NAME(CookPixelName, "CookPixel");
ROCKS_SHADER_TYPE_NAME(CookWaveName, "CookWave");
ROCKS_SHADER_TYPE_NAME(CookEngineName, "CookEngine");
ROCKS_SHADER_TYPE_NAME(CookBrokenName, "CookBroken");
ROCKS_SHADER_TYPE_NAME(VertexInputAName, "CookVertexA");
ROCKS_SHADER_TYPE_NAME(VertexInputBName, "CookVertexB");

constexpr ShaderPermutationDimension CookScale[] = {
    ShaderPermutationDimension::IntRange("SCALE", 1, 3)};
constexpr ShaderPermutationDimension CookSingleScale[] = {
    ShaderPermutationDimension::IntRange("SCALE", 1, 1)};
constexpr ShaderPermutationDimension CookDebug[] = {
    ShaderPermutationDimension::Bool("USE_DEBUG", true)};
constexpr ShaderParameterMember CookPassMembers[] = {{"tint", ShaderParameterType::Float4, 0, 0}};
constexpr ShaderParameterLayout CookPixelLayouts[] = {
    {"PassConstants", false, 16, CookPassMembers}};

// Vulkan에서 SCALE = 3(퍼뮤테이션 2)을 뺀다.
bool CookComputeFilter(VertexInputTypeId, uint32 permutationId,
                       const ShaderFormatFacts& format) noexcept {
    return format.backend != ShaderBackend::Vulkan || permutationId != 2;
}

// 정점 입력 B는 Vulkan에서 뺀다.
bool CookVertexBFilter(const ShaderTypeDesc&, uint32, const ShaderFormatFacts& format) noexcept {
    return format.backend != ShaderBackend::Vulkan;
}

constexpr ShaderTypeDesc MakeCookCompute() noexcept {
    ShaderTypeDesc desc = MakeShaderTypeDesc(CookComputeName, CookEntry, "CSMain",
                                             ShaderStageCompute, ShaderTypeOwner::Project);
    desc.requiredFeatures = ShaderFeatureCompute;
    desc.permutation = ShaderPermutationDomain{CookScale};
    desc.shouldCompile = &CookComputeFilter;
    return desc;
}

constexpr ShaderTypeDesc MakeCookPixel() noexcept {
    ShaderTypeDesc desc = MakeShaderTypeDesc(CookPixelName, CookEntry, "PSMain", ShaderStagePixel,
                                             ShaderTypeOwner::Project);
    desc.requiredFeatures = ShaderFeatureVertexViewportLayerOutput;
    desc.flags = ShaderTypeVertexInputAxis;
    desc.permutation = ShaderPermutationDomain{CookDebug};
    desc.parameterLayouts = CookPixelLayouts;
    return desc;
}

constexpr ShaderTypeDesc MakeCookWave() noexcept {
    ShaderTypeDesc desc = MakeShaderTypeDesc(CookWaveName, CookEntry, "CSMain", ShaderStageCompute,
                                             ShaderTypeOwner::Project);
    desc.requiredFeatures = ShaderFeatureCompute | ShaderFeatureWaveOperations;
    desc.permutation = ShaderPermutationDomain{CookSingleScale};
    return desc;
}

constexpr ShaderTypeDesc MakeCookBroken() noexcept {
    ShaderTypeDesc desc = MakeShaderTypeDesc(CookBrokenName, CookEntry, "NoSuchEntry",
                                             ShaderStageCompute, ShaderTypeOwner::Project);
    desc.permutation = ShaderPermutationDomain{CookSingleScale};
    return desc;
}

constexpr ShaderTypeDesc CookCompute = MakeCookCompute();
constexpr ShaderTypeDesc CookPixel = MakeCookPixel();
constexpr ShaderTypeDesc CookWave = MakeCookWave();
constexpr ShaderTypeDesc CookBroken = MakeCookBroken();
// Engine 소속은 Project 라이브러리에 들지 않는다(원문이 없어도 열거되지 않으므로 컴파일하지
// 않는다).
constexpr ShaderTypeDesc CookEngine =
    MakeShaderTypeDesc(CookEngineName, "/Engine/Private/NotCooked.rsf", "Main", ShaderStagePixel,
                       ShaderTypeOwner::Engine);

constexpr ShaderVertexInputType CookVertexInputs[] = {
    {VertexInputBName.hash, VertexInputBName.name, &CookVertexBFilter},
    {VertexInputAName.hash, VertexInputAName.name, nullptr}};

// 쿠킹 결과 파일을 연다.
bool OpenCooked(const ShaderLibraryCookFormat& line, Array<uint8>& bytes,
                ShaderLibraryView& view) noexcept {
    bool existed = false;
    SHADER_MANAGER_TEST_REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), line.path.View(),
                                              NumericLimits<uint32>::Max(), bytes, existed));
    SHADER_MANAGER_TEST_REQUIRE(existed);
    const ShaderReadStatus opened = view.Open(bytes.View());
    SHADER_MANAGER_TEST_REQUIRE(opened);
    return true;
}

// 파일을 bytes로 덮어쓴다(손상 흉내).
bool OverwriteFile(const String& path, const Array<uint8>& bytes) noexcept {
    platform::File file;
    SHADER_MANAGER_TEST_REQUIRE(file.Open(path.CStr(), platform::FileOpenMode::CreateAlways));
    SHADER_MANAGER_TEST_REQUIRE(file.WriteAll(bytes.Data(), bytes.Size()).status);
    SHADER_MANAGER_TEST_REQUIRE(file.Close());
    return true;
}

bool HasEntry(const ShaderLibraryView& view, const ShaderTypeName& type,
              VertexInputTypeId vertexInput, uint32 permutation, ShaderLibraryEntry& out) noexcept {
    return view.Find(ShaderLibraryKey{0, type.hash, vertexInput, permutation}, out);
}

} // namespace

TEST(ShaderLibraryCook_CooksFiltersAndPublishes) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("librarycook", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderLibraryCook_CooksFiltersAndPublishes: %s\n", skip);
        }

        return;
    }

    REQUIRE(test.fixture.Write("Project/Shaders/Cook.rsf", CookSource));

    ShaderTypeRegistry registry(TestAllocator());
    const ShaderTypeDesc* const types[] = {&CookCompute, &CookPixel, &CookWave, &CookEngine};
    REQUIRE(registry.Register(types));
    ShaderCompileManagerConfig config = test.Config("CookWork");
    ShaderCompileManager manager(config);
    REQUIRE(manager.Start());
    char output[ManagerPathBytes];
    REQUIRE(test.fixture.Child("Cooked/Shaders", output, sizeof(output)));
    const ShaderFormatSnapshot* const formats[] = {&test.d3d12, &test.vulkan};
    ShaderLibraryCookDesc desc;
    desc.allocator = &TestAllocator();
    desc.registry = &registry;
    desc.formats = formats;
    desc.vertexInputTypes = CookVertexInputs;
    desc.library = ShaderLibraryKind::Project;
    desc.manager = &manager;
    desc.outputDirectory = StringView(output);
    ShaderLibraryCookReport report(TestAllocator());
    // Freeze 전에는 NotReady다.
    const Result<void> early = CookShaderLibraries(desc, report);
    CHECK(!early && early.Error() == ErrorCode::NotReady);
    REQUIRE(registry.Freeze());
    // 정점 입력 id가 이름 해시가 아니면 InvalidArgument다.
    const ShaderVertexInputType badInputs[] = {{1, "CookVertexA", nullptr}};
    desc.vertexInputTypes = badInputs;
    const Result<void> invalid = CookShaderLibraries(desc, report);
    CHECK(!invalid && invalid.Error() == ErrorCode::InvalidArgument);
    desc.vertexInputTypes = CookVertexInputs;

    const Result<void> cooked = CookShaderLibraries(desc, report);
    for (const ShaderLibraryCookJobFailure& failure : report.failures) {
        fprintf(stderr, "cook job %s %s %u: %s\n", failure.formatId, failure.typeName,
                failure.permutationId, failure.diagnostics.Data());
    }

    REQUIRE(cooked);
    REQUIRE(report.formats.Size() == 2);
    const ShaderLibraryCookFormat& d3d12 = report.formats[0];
    const ShaderLibraryCookFormat& vulkan = report.formats[1];
    CHECK(StringView(d3d12.formatId) == "D3D12_SM6" && StringView(vulkan.formatId) == "VULKAN_SM5");
    CHECK(d3d12.outcome == ShaderLibraryCookOutcome::Written &&
          vulkan.outcome == ShaderLibraryCookOutcome::Written);
    // D3D12: Compute 3 + Pixel 2(정점 입력 A·B, 개발 전용 퍼뮤테이션 제외) + Wave 1. Vulkan:
    // Compute 2 (거르기) + Pixel 1(정점 입력 B 거르기), Wave는 기능이 없어 빠진다.
    CHECK(d3d12.jobs == 6 && d3d12.entries == 6 && d3d12.failedJobs == 0);
    CHECK(vulkan.jobs == 3 && vulkan.entries == 3);
    // 같은 원문·정의는 같은 코드다: Wave = Compute SCALE 1, Pixel A = Pixel B.
    CHECK(d3d12.codes == 4 && vulkan.codes == 3);

    char hex[Sha256Digest::HexBytes + 1];
    d3d12.fileSha256.ToHex(hex);
    char expectedName[ManagerPathBytes];
    snprintf(expectedName, sizeof(expectedName), "%s/Project-D3D12_SM6-%.16s.rshl", output, hex);
    CHECK(d3d12.path.View() == StringView(expectedName));

    Array<uint8> bytes(TestAllocator());
    ShaderLibraryView view;
    REQUIRE(OpenCooked(d3d12, bytes, view));
    CHECK(view.Kind() == ShaderLibraryKind::Project && view.FileSha256() == d3d12.fileSha256);
    CHECK(view.Facts().FormatId() == StringView("D3D12_SM6") &&
          view.Facts().backend == ShaderBackend::D3D12 &&
          view.Facts().features == test.d3d12.definition.features);
    CHECK(view.FormatDefinitionSha256() == test.d3d12.definition.sha256);
    CHECK(view.TypeSetSha256() == registry.TypeSetDigest());
    ShaderLibraryEntry entry;
    for (uint32 permutation = 0; permutation < 3; ++permutation) {
        CHECK(HasEntry(view, CookComputeName, NoVertexInputType, permutation, entry));
        CHECK(entry.stage == ShaderStageCompute && !entry.developmentOnly &&
              entry.layoutSignature == Sha256Digest{} && entry.runtimeFeatures == 0);
        CHECK(!view.Value(entry, ShaderLibraryValueCode).IsEmpty());
    }

    ShaderLibraryEntry wave;
    ShaderLibraryEntry computeOne;
    REQUIRE(HasEntry(view, CookWaveName, NoVertexInputType, 0, wave));
    REQUIRE(HasEntry(view, CookComputeName, NoVertexInputType, 0, computeOne));
    CHECK(wave.codeIndex == computeOne.codeIndex);
    ShaderLibraryEntry pixelA;
    ShaderLibraryEntry pixelB;
    REQUIRE(HasEntry(view, CookPixelName, VertexInputAName.hash, 0, pixelA));
    REQUIRE(HasEntry(view, CookPixelName, VertexInputBName.hash, 0, pixelB));
    CHECK(pixelA.codeIndex == pixelB.codeIndex);
    CHECK(pixelA.runtimeFeatures == ShaderFeatureVertexViewportLayerOutput &&
          pixelA.layoutSignature == ShaderParameterLayoutSignature(CookPixelLayouts));
    CHECK(!HasEntry(view, CookPixelName, VertexInputAName.hash, 1, entry));
    CHECK(!HasEntry(view, CookEngineName, NoVertexInputType, 0, entry));
    CHECK(view.TypeName(CookPixelName.hash) == StringView("CookPixel"));

    Array<uint8> vulkanBytes(TestAllocator());
    ShaderLibraryView vulkanView;
    REQUIRE(OpenCooked(vulkan, vulkanBytes, vulkanView));
    CHECK(vulkanView.Facts().backend == ShaderBackend::Vulkan);
    CHECK(!HasEntry(vulkanView, CookComputeName, NoVertexInputType, 2, entry));
    CHECK(!HasEntry(vulkanView, CookPixelName, VertexInputBName.hash, 0, entry));
    CHECK(!HasEntry(vulkanView, CookWaveName, NoVertexInputType, 0, entry));
    CHECK(HasEntry(vulkanView, CookPixelName, VertexInputAName.hash, 0, entry));

    // 다시 쿠킹하면 같은 byte라 그대로 둔다(너겟 적중).
    ShaderLibraryCookReport again(TestAllocator());
    REQUIRE(CookShaderLibraries(desc, again));
    REQUIRE(again.formats.Size() == 2);
    CHECK(again.formats[0].outcome == ShaderLibraryCookOutcome::Unchanged &&
          again.formats[1].outcome == ShaderLibraryCookOutcome::Unchanged);
    CHECK(again.formats[0].path.View() == d3d12.path.View() &&
          again.formats[0].fileSha256 == d3d12.fileSha256);

    // 같은 이름의 다른 byte는 손대지 않고 CorruptEntry다.
    bytes[bytes.Size() - 1] ^= 0x01;
    REQUIRE(OverwriteFile(d3d12.path, bytes));
    ShaderLibraryCookReport corrupt(TestAllocator());
    const Result<void> collided = CookShaderLibraries(desc, corrupt);
    CHECK(!collided && collided.Error() == ErrorCode::AlreadyExists);
    REQUIRE(corrupt.formats.Size() == 2);
    CHECK(corrupt.formats[0].outcome == ShaderLibraryCookOutcome::CorruptEntry &&
          corrupt.formats[1].outcome == ShaderLibraryCookOutcome::Unchanged);
    Array<uint8> kept(TestAllocator());
    bool existed = false;
    REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), d3d12.path.View(), bytes.Size(), kept,
                          existed));
    CHECK(kept.Size() == bytes.Size() && memcmp(kept.Data(), bytes.Data(), bytes.Size()) == 0);

    // 개발 전용을 담으면 다른 파일이고 개발 전용 항목이 표시된다.
    desc.includeDevelopment = true;
    ShaderLibraryCookReport development(TestAllocator());
    REQUIRE(CookShaderLibraries(desc, development));
    CHECK(development.formats[0].outcome == ShaderLibraryCookOutcome::Written &&
          development.formats[0].entries == 8 && development.formats[1].entries == 4);
    CHECK(!(development.formats[0].path.View() == d3d12.path.View()));
    Array<uint8> developmentBytes(TestAllocator());
    ShaderLibraryView developmentView;
    REQUIRE(OpenCooked(development.formats[0], developmentBytes, developmentView));
    REQUIRE(HasEntry(developmentView, CookPixelName, VertexInputAName.hash, 1, entry));
    CHECK(entry.developmentOnly);
    REQUIRE(HasEntry(developmentView, CookPixelName, VertexInputAName.hash, 0, entry));
    CHECK(!entry.developmentOnly);
    manager.Shutdown();
}

TEST(ShaderLibraryCook_JobFailuresKeepTheFormatUnwritten) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("librarycookfail", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderLibraryCook_JobFailuresKeepTheFormatUnwritten: %s\n", skip);
        }

        return;
    }

    REQUIRE(test.fixture.Write("Project/Shaders/Cook.rsf", CookSource));

    ShaderTypeRegistry registry(TestAllocator());
    const ShaderTypeDesc* const types[] = {&CookCompute, &CookBroken};
    REQUIRE(registry.Register(types));
    REQUIRE(registry.Freeze());
    ShaderCompileManager manager(test.Config("CookFailWork"));
    REQUIRE(manager.Start());
    char output[ManagerPathBytes];
    REQUIRE(test.fixture.Child("Cooked/Shaders", output, sizeof(output)));
    const ShaderFormatSnapshot* const formats[] = {&test.d3d12};
    ShaderLibraryCookDesc desc;
    desc.allocator = &TestAllocator();
    desc.registry = &registry;
    desc.formats = formats;
    desc.library = ShaderLibraryKind::Project;
    desc.manager = &manager;
    desc.outputDirectory = StringView(output);
    ShaderLibraryCookReport report(TestAllocator());
    const Result<void> cooked = CookShaderLibraries(desc, report);
    CHECK(!cooked && cooked.Error() == ErrorCode::DependencyFailed);
    REQUIRE(report.formats.Size() == 1);
    CHECK(report.formats[0].outcome == ShaderLibraryCookOutcome::JobsFailed &&
          report.formats[0].jobs == 4 && report.formats[0].failedJobs == 1 &&
          report.formats[0].path.IsEmpty());
    REQUIRE(report.failures.Size() == 1);
    CHECK(StringView(report.failures[0].typeName) == "CookBroken" &&
          StringView(report.failures[0].formatId) == "D3D12_SM6" &&
          report.failures[0].status == ShaderCompileStatus::CompileError &&
          !report.failures[0].diagnostics.IsEmpty());
    // 출력 디렉터리를 만들지도 않았다.
    bool exists = true;
    REQUIRE(platform::PhysicalFileSystem().DirectoryExists(output, strlen(output), exists));
    CHECK(!exists);
    CHECK(StringView(ShaderLibraryCookOutcomeName(ShaderLibraryCookOutcome::CorruptEntry)) ==
          "CorruptEntry");
    manager.Shutdown();
}

} // namespace shader_compile_manager_tests_file
