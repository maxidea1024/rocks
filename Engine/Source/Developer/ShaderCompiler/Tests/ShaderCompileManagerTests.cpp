#include "ShaderSourceTestSupport.h"

#include <AssetNugget/NuggetCache.h>
#include <Core/Text/String.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompiler/ShaderCompileCommandlet.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCompiler/ShaderWorkerChannel.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <Task/WorkerScheduler.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 셰이더 컴파일 관리자와 shader-compile 명령 시험이다(Docs/Design/ShaderSystem 10절, 11.1절
// SC4·SC11–SC13, SH1d): 너겟 조회 결과 분류, 설정 검사, 다른 PID 작업 디렉터리 정리, 같은 입력의
// 같은 byte(워커 수·묶음 크기·작업 디렉터리와 무관), 찬 캐시(워커 0개), 값 0 없는 Record의
// 재컴파일, 실패 묶음 격리, 진행 계수 정산, 명령 출력·종료 코드·출력 파일 이름. 워커가 없거나 도구
// 저장소가 준비되지 않았으면 그 시험만 SKIP이다.
using namespace rocks;

namespace shader_compile_manager_tests_file {

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

namespace {

constexpr usize ManagerPathBytes = 4096;
// 확실히 살아 있지 않은 PID다(Windows PID는 4의 배수, POSIX pid_max보다 크다).
constexpr char DeadPidName[] = "4294967294";
// 시험 레지스트리 문맥 id다("SHMT").
constexpr uint32 TestContextId = 0x53484d54;

constexpr char TestSource[] =
    "RWStructuredBuffer<uint> Output;\n"
    "[numthreads(4, 1, 1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID) { Output[id.x] = id.x * SCALE; }\n"
    "float4 PSMain() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";
constexpr char TestEntry[] = "/Project/Test.rsf";
constexpr char NuggetSettings[] = "{ \"localPath\": \"Cache/Shader\", \"required\": true, "
                                  "\"generation\": \"01234567-89ab-cdef-0123-456789abcdef\" }";

// 시험 실행 파일 옆의 워커다.
bool WorkerExecutable(char (&out)[ManagerPathBytes]) noexcept {
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
bool ToolsReady(const char* worker, const Fixture& fixture) noexcept {
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
        config.profileId = "Engine-Windows";
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
void PrintFailures(const Array<ShaderCompileJobResult>& results, usize count) noexcept {
    for (usize index = 0; index < count && index < results.Size(); ++index) {
        if (results[index].status != ShaderCompileStatus::Success) {
            fprintf(stderr, "job %zu %s: %.*s\n", index,
                    ShaderCompileStatusName(results[index].status),
                    static_cast<int>(results[index].diagnostics.ByteCount()),
                    results[index].diagnostics.Data());
        }
    }
}

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
    config.profileId = "Engine-Windows";
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

} // namespace shader_compile_manager_tests_file

#undef SHADER_MANAGER_TEST_REQUIRE
