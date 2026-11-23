#include "ShaderSourceTestSupport.h"

#include <Core/Text/String.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompiler/ShaderWorkerChannel.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>
#include <Base/InitializerList.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 셰이더 워커 채널과 실제 ShaderCompileWorker 시험이다(Docs/Design/ShaderSystem.ko.md 11.1절
// SC4–SC7, SH1c): 버전·처리기·도구 표·툴체인 불일치의 종료 코드와 진단, 도구 조회,
// D3D12_SM6·VULKAN_SM5 컴파일 (도구 저장소가 없으면 그 부분만 SKIP), hook 없는 워커의 hook 거부,
// 강제 종료 보고(held gate), Crash·Assert 보고와 다음 묶음 성공(대화상자 없음 — 시험이 끝난다),
// 부모 비정상 종료 시 워커와 손자 종료. 워커 실행 파일은 시험 실행 파일 옆에 동반 타깃으로
// 빌드된다(SC14). 없으면 SKIP이다.
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace shader_worker_channel_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

namespace {

constexpr usize ChannelPathBytes = 4096;
// 부모 probe가 스스로 끝나는 종료 코드다(정리 없이 _Exit).
constexpr int ParentDiedExit = 3;
constexpr int ProbeUsageExit = 2;
// 프로세스 종료 기다림의 실패 판정 상한이다(정상 경로는 OS 대기로 바로 끝난다).
constexpr uint32 ExitWaitMilliseconds = 60'000;

constexpr char ComputeSource[] =
    "RWStructuredBuffer<uint> Output;\n"
    "[numthreads(4, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) { Output[id.x] = id.x * 2; "
    "}\n";
constexpr char BrokenSource[] = "[numthreads(1, 1, 1)]\nvoid main() { undefined_name_c; }\n";

// 시험 실행 파일 옆의 워커 실행 파일이다.
bool WorkerExecutable(char (&out)[ChannelPathBytes]) noexcept {
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

bool ReadConfig(const char* id, Array<uint8>& bytes) noexcept {
    char path[ChannelPathBytes];
    snprintf(path, sizeof(path), "%s/%s.json", ShaderFormatDirectory, id);
    platform::File file;
    uint64 size = 0;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting) || !file.Size(size) ||
        !bytes.TryResize(static_cast<usize>(size))) {
        return false;
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    return static_cast<bool>(file.Close()) && static_cast<bool>(read.status);
}

// 요청 한 벌이다(형식 정의는 저장소 파일 byte).
struct RequestBuilder {
    RequestBuilder() noexcept : request(TestAllocator()), definitions(TestAllocator()) {
    }

    bool AddFormat(const char* id) noexcept {
        auto format = request.formats.TryEmplaceBack(TestAllocator());
        ShaderFormatDefinition definition;
        return format && format.Value()->id.TryAssign(id) &&
               ReadConfig(id, format.Value()->bytes) &&
               ParseShaderFormatDefinition(format.Value()->bytes.View(), id, definition) &&
               definitions.TryPushBack(definition);
    }

    bool AddJob(ShaderWorkerJobKind kind, const char* format, uint32 stage,
                const char* source) noexcept {
        auto job = request.jobs.TryEmplaceBack(TestAllocator());
        return job && (job.Value()->kind = kind, true) && job.Value()->formatId.TryAssign(format) &&
               (job.Value()->stage = stage, true) && job.Value()->entry.TryAssign("main") &&
               job.Value()->source.TryAssign(source);
    }

    bool Encode(Array<uint8>& bytes) noexcept {
        Array<const ShaderFormatDefinition*> pointers(TestAllocator());
        for (const ShaderFormatDefinition& definition : definitions) {
            if (!pointers.TryPushBack(&definition)) {
                return false;
            }
        }

        return MakeShaderWorkerHeader(pointers.View(), request.header) &&
               EncodeShaderWorkerRequest(request, bytes);
    }

    ShaderWorkerRequest request;
    Array<ShaderFormatDefinition> definitions;
};

// 워커 채널 한 벌(임시 작업 디렉터리)이다.
struct ChannelCase {
    ChannelCase() noexcept : channel(TestAllocator()), result(TestAllocator()) {
    }

    // 워커가 없으면 false(SKIP)다. ready에 준비 성공 여부를 둔다.
    bool Make(const char* tag, bool testHooks, bool& ready,
              const ShaderWorkerChannelHooks& hooks = {}) noexcept {
        ready = false;
        char worker[ChannelPathBytes];
        if (!WorkerExecutable(worker)) {
            return false;
        }

        char directory[FixturePathBytes];
        if (!fixture.Make(tag) || !fixture.Child("w", directory, sizeof(directory))) {
            return true;
        }

        ShaderWorkerLaunch launch;
        launch.executable = StringView(worker, strlen(worker));
        launch.workDirectory = StringView(directory, strlen(directory));
        launch.testHooks = testHooks;
        ready = static_cast<bool>(channel.Start(launch, hooks));
        return true;
    }

    bool Run(RequestBuilder& builder) noexcept {
        Array<uint8> bytes(TestAllocator());
        if (!builder.Encode(bytes)) {
            return false;
        }

        channel.Run(bytes.View(), result);
        return true;
    }

    Fixture fixture;
    ShaderWorkerChannel channel;
    ShaderWorkerBatchResult result;
};

void PrintResult(const char* what, const ShaderWorkerBatchResult& result) noexcept {
    fprintf(stderr, "%s: outcome %s exit kind %u code %u diagnostics %.*s\n", what,
            ShaderWorkerBatchOutcomeName(result.outcome), static_cast<unsigned>(result.exit.kind),
            result.exit.code, static_cast<int>(result.diagnostics.ByteCount()),
            result.diagnostics.Data());
}

bool ExitedWith(const ShaderWorkerBatchResult& result, ShaderWorkerExit code) noexcept {
    const bool matched = result.outcome == ShaderWorkerBatchOutcome::WorkerExited &&
                         result.exit.kind == platform::ProcessExitKind::Exited &&
                         result.exit.code == static_cast<uint32>(code);
    if (!matched) {
        PrintResult(ShaderWorkerExitName(static_cast<int32>(code)), result);
    }

    return matched;
}

ShaderWorkerHeldAction TerminateHeld(void*, uint32, uint32, Span<const uint32>) noexcept {
    return ShaderWorkerHeldAction::Terminate;
}

// 부모 probe다: 워커를 --test-hooks로 띄워 SpawnChild(손자 = 이 실행 파일의 sleep probe)를 맡기고,
// 워커·손자 PID를 표준 출력에 쓴 뒤 표준 입력의 한 줄을 기다려 정리 없이(_Exit) 끝난다.
// argv: <exe> --process-probe shader-worker-parent <worker> <work-dir>
int ParentProbe(int argc, const char* const* argv) noexcept {
    constexpr int WorkerArgument = 3;
    constexpr int DirectoryArgument = 4;
    if (argc <= DirectoryArgument) {
        return ProbeUsageExit;
    }

    const char* worker = argv[WorkerArgument];
    const char* directory = argv[DirectoryArgument];
    char self[ChannelPathBytes];
    usize selfBytes = 0;
    RequestBuilder builder;
    if (!platform::GetExecutablePath(self, sizeof(self), selfBytes) ||
        !builder.AddJob(ShaderWorkerJobKind::SpawnChild, "", ShaderStageCompute, "")) {
        return ProbeUsageExit;
    }

    for (const char* argument :
         {static_cast<const char*>(self), "--process-probe", "sleep", "600000"}) {
        auto added = builder.request.jobs[0].hookArguments.TryEmplaceBack(TestAllocator());
        if (!added || !added.Value()->TryAssign(argument)) {
            return ProbeUsageExit;
        }
    }

    Array<uint8> bytes(TestAllocator());
    char input[ChannelPathBytes];
    snprintf(input, sizeof(input), "%s/input.1", directory);
    platform::File file;
    if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), directory,
                                     strlen(directory)) ||
        !builder.Encode(bytes) || !file.Open(input, platform::FileOpenMode::CreateAlways) ||
        !file.WriteAll(bytes.Data(), bytes.Size()).status || !file.Close()) {
        return ProbeUsageExit;
    }

    char parent[16];
    snprintf(parent, sizeof(parent), "%u", platform::CurrentProcessId());
    const platform::ProcessArgument arguments[] = {
        {"--work-dir", 10},   {directory, strlen(directory)},
        {"--parent-pid", 12}, {parent, strlen(parent)},
        {"--worker", 8},      {"0", 1},
        {"--test-hooks", 12}};
    platform::ProcessStartInfo info;
    info.executable = worker;
    info.executableBytes = strlen(worker);
    info.arguments = arguments;
    info.argumentCount = sizeof(arguments) / sizeof(arguments[0]);
    info.input = platform::ProcessStreamMode::Pipe;
    info.output = platform::ProcessStreamMode::Pipe;
    info.error = platform::ProcessStreamMode::Null;
    platform::Process process;
    if (!process.Start(info) || !process.WriteInput("1\n", 2).status) {
        return ProbeUsageExit;
    }

    // `child <pid>`와 `held 1`을 기다린다.
    char lines[256] = {};
    usize received = 0;
    while (strstr(lines, "held 1") == nullptr) {
        const platform::FileTransfer read = process.Read(
            platform::ProcessStream::Output, lines + received, sizeof(lines) - 1 - received);
        if (!read.status || read.bytes == 0) {
            return ProbeUsageExit;
        }

        received += read.bytes;
    }

    const char* child = strstr(lines, "child ");
    if (child == nullptr) {
        return ProbeUsageExit;
    }

    char report[96];
    const int written = snprintf(report, sizeof(report), "worker %u\nchild %lu\n", process.Id(),
                                 strtoul(child + 6, nullptr, 10));
    if (written <= 0 || !test::WriteStandardStream(1, report, static_cast<usize>(written))) {
        return ProbeUsageExit;
    }

    char gate = 0;
    (void)test::ReadStandardInput(&gate, 1);
    // 정리 없이 끝난다(Process 소멸자가 워커를 끝내지 않게 — 부모 비정상 종료 모사).
    _Exit(ParentDiedExit);
}

const bool ParentProbeRegistered =
    rocks::test::RegisterProcessProbe("shader-worker-parent", &ParentProbe);

// 자식 출력에서 "<name> <숫자>" 줄의 숫자를 읽는다.
uint32 ReportedId(const char* text, const char* name) noexcept {
    char key[16];
    snprintf(key, sizeof(key), "%s ", name);
    const char* found = strstr(text, key);
    return found == nullptr ? 0u : static_cast<uint32>(strtoul(found + strlen(key), nullptr, 10));
}

} // namespace

TEST(ShaderWorkerChannel_ReportsVersionMismatches) {
    ChannelCase test;
    bool ready = false;
    if (!test.Make("workerversion", false, ready)) {
        printf(
            "[SKIP] ShaderWorkerChannel_ReportsVersionMismatches: ShaderCompileWorker is not next "
            "to the test executable\n");
        return;
    }

    REQUIRE(ready);
    RequestBuilder base;
    REQUIRE(base.AddFormat("D3D12_SM6"));
    Array<uint8> good(TestAllocator());
    REQUIRE(base.Encode(good));

    // 메시지 버전·magic·잘림.
    {
        Array<uint8> bytes(TestAllocator());
        REQUIRE(bytes.TryAppend(good.View()));
        bytes[ShaderWorkerMagicBytes] ^= 0x7f;
        test.channel.Run(bytes.View(), test.result);
        CHECK(ExitedWith(test.result, ShaderWorkerExit::BadMessageVersion));
        CHECK(strstr(test.result.diagnostics.Data(), "rebuild ShaderCompileWorker") != nullptr);
        bytes[ShaderWorkerMagicBytes] ^= 0x7f;
        bytes[0] = 'X';
        test.channel.Run(bytes.View(), test.result);
        CHECK(ExitedWith(test.result, ShaderWorkerExit::BadInputFile));
        test.channel.Run(good.View().Left(ShaderWorkerPrefixBytes + 9), test.result);
        CHECK(ExitedWith(test.result, ShaderWorkerExit::BadInputFile));
    }

    // 툴체인 지문·결정적 수학 버전·도구 표.
    for (int field = 0; field < 3; ++field) {
        RequestBuilder builder;
        REQUIRE(builder.AddFormat("D3D12_SM6"));
        Array<uint8> bytes(TestAllocator());
        Array<const ShaderFormatDefinition*> pointers(TestAllocator());
        REQUIRE(pointers.TryPushBack(&builder.definitions[0]));
        REQUIRE(MakeShaderWorkerHeader(pointers.View(), builder.request.header));
        if (field == 0) {
            builder.request.header.toolchain.bytes[0] ^= 0xff;
        } else if (field == 1) {
            ++builder.request.header.mathVersion;
        } else {
            builder.request.header.toolTable.bytes[31] ^= 0xff;
        }

        REQUIRE(EncodeShaderWorkerRequest(builder.request, bytes));
        test.channel.Run(bytes.View(), test.result);
        if (field < 2) {
            CHECK(ExitedWith(test.result, ShaderWorkerExit::BadToolchain));
            CHECK(strstr(test.result.diagnostics.Data(), "rebuild ShaderCompileWorker") != nullptr);
        } else {
            CHECK(ExitedWith(test.result, ShaderWorkerExit::BadToolTable));
            CHECK(strstr(test.result.diagnostics.Data(), "tool table") != nullptr);
        }
    }

    // 처리기 버전: 형식 정의 processor.version을 2로 올린 요청은 워커의 D3D 처리기(1)와 다르다.
    {
        RequestBuilder builder;
        auto format = builder.request.formats.TryEmplaceBack(TestAllocator());
        REQUIRE(format && format.Value()->id.TryAssign("D3D12_SM6"));
        Array<uint8> original(TestAllocator());
        REQUIRE(ReadConfig("D3D12_SM6", original));
        const char needle[] = "\"version\": 1";
        const char* text = reinterpret_cast<const char*>(original.Data());
        usize at = IndexNone;
        for (usize index = 0; index + sizeof(needle) - 1 <= original.Size(); ++index) {
            if (memcmp(text + index, needle, sizeof(needle) - 1) == 0) {
                at = index;
                break;
            }
        }

        REQUIRE(at != IndexNone);
        REQUIRE(format.Value()->bytes.TryAppend(original.View()));
        format.Value()->bytes[at + sizeof(needle) - 2] = '2';
        ShaderFormatDefinition definition;
        REQUIRE(ParseShaderFormatDefinition(format.Value()->bytes.View(), "D3D12_SM6", definition));
        REQUIRE(definition.moduleVersion == 2);
        const ShaderFormatDefinition* pointers[] = {&definition};
        REQUIRE(MakeShaderWorkerHeader(pointers, builder.request.header));
        Array<uint8> bytes(TestAllocator());
        REQUIRE(EncodeShaderWorkerRequest(builder.request, bytes));
        test.channel.Run(bytes.View(), test.result);
        CHECK(ExitedWith(test.result, ShaderWorkerExit::BadFormatVersion));
        CHECK(strstr(test.result.diagnostics.Data(), "rebuild ShaderCompileWorker") != nullptr);
    }

    // 끝난 워커 뒤의 묶음은 새 워커가 처리한다.
    test.channel.Run(good.View(), test.result);
    CHECK(test.result.outcome == ShaderWorkerBatchOutcome::Completed);
    test.channel.Shutdown();
    test.channel.Run(good.View(), test.result);
    CHECK(test.result.outcome == ShaderWorkerBatchOutcome::ShutDown);
}

TEST(ShaderWorkerChannel_QueriesToolsAndCompiles) {
    ChannelCase test;
    bool ready = false;
    if (!test.Make("workercompile", false, ready)) {
        printf(
            "[SKIP] ShaderWorkerChannel_QueriesToolsAndCompiles: ShaderCompileWorker is not next "
            "to the test executable\n");
        return;
    }

    REQUIRE(ready);
    RequestBuilder query;
    query.request.kind = ShaderWorkerRequestKind::ToolQuery;
    REQUIRE(test.Run(query));
    REQUIRE(test.result.outcome == ShaderWorkerBatchOutcome::Completed);
    bool bootstrapped = !test.result.response.tools.IsEmpty();
    bool listsDxc = false;
    for (const ShaderWorkerToolCheck& tool : test.result.response.tools) {
        listsDxc = listsDxc || tool.name.View() == StringView(DxcToolName);
        bootstrapped = bootstrapped && tool.state == ShaderWorkerToolState::Ready;
    }

    CHECK(listsDxc);
    // system 도구(MetalToolchain)는 지문 칸으로 따로 온다.
    bool listsMetal = false;
    for (const ShaderWorkerSystemTool& tool : test.result.response.systemTools) {
        listsMetal = listsMetal || tool.name.View() == StringView(MetalToolName);
    }

    CHECK(listsMetal);

    // hook job은 --test-hooks 없는 워커에서 ToolError다(워커는 계속 돈다).
    RequestBuilder hook;
    REQUIRE(hook.AddJob(ShaderWorkerJobKind::Crash, "", ShaderStageCompute, ""));
    REQUIRE(test.Run(hook));
    REQUIRE(test.result.outcome == ShaderWorkerBatchOutcome::Completed);
    REQUIRE(test.result.response.jobs.Size() == 1);
    CHECK(test.result.response.jobs[0].status == ShaderWorkerJobStatus::ToolError);

    // 아직 처리기가 없는 형식(GLES)은 UnsupportedCapability다(도구와 무관).
    RequestBuilder gles;
    REQUIRE(gles.AddFormat("GLES31_PC"));
    REQUIRE(
        gles.AddJob(ShaderWorkerJobKind::Compile, "GLES31_PC", ShaderStageCompute, ComputeSource));
    REQUIRE(test.Run(gles));
    REQUIRE(test.result.outcome == ShaderWorkerBatchOutcome::Completed);
    CHECK(test.result.response.jobs[0].status == ShaderWorkerJobStatus::UnsupportedCapability);

    if (!bootstrapped) {
        printf("[SKIP] ShaderWorkerChannel_QueriesToolsAndCompiles: shader tool store is not "
               "bootstrapped (dotnet msbuild Rocks.proj /t:ThirdPartyBootstrap)\n");
        return;
    }

    RequestBuilder compile;
    REQUIRE(compile.AddFormat("D3D12_SM6") && compile.AddFormat("VULKAN_SM5") &&
            compile.AddFormat("D3D11_SM5"));
    REQUIRE(compile.AddJob(ShaderWorkerJobKind::Compile, "D3D12_SM6", ShaderStageCompute,
                           ComputeSource));
    REQUIRE(compile.AddJob(ShaderWorkerJobKind::Compile, "VULKAN_SM5", ShaderStageCompute,
                           ComputeSource));
    REQUIRE(compile.AddJob(ShaderWorkerJobKind::Compile, "D3D12_SM6", ShaderStageCompute,
                           BrokenSource));
    REQUIRE(compile.AddJob(ShaderWorkerJobKind::Compile, "D3D11_SM5", ShaderStageCompute,
                           ComputeSource));
    REQUIRE(test.Run(compile));
    REQUIRE(test.result.outcome == ShaderWorkerBatchOutcome::Completed);
    REQUIRE(test.result.response.jobs.Size() == 4);
    const ShaderWorkerJobResult& dxil = test.result.response.jobs[0];
    const ShaderWorkerJobResult& spirv = test.result.response.jobs[1];
    if (dxil.status != ShaderWorkerJobStatus::Success ||
        spirv.status != ShaderWorkerJobStatus::Success) {
        fprintf(stderr, "DXIL: %.*s\nSPIR-V: %.*s\n",
                static_cast<int>(dxil.diagnostics.ByteCount()), dxil.diagnostics.Data(),
                static_cast<int>(spirv.diagnostics.ByteCount()), spirv.diagnostics.Data());
    }

    REQUIRE(dxil.status == ShaderWorkerJobStatus::Success && dxil.values.Size() == 1);
    CHECK(dxil.values[0].id == 0 && dxil.values[0].bytes.Size() > 20 &&
          memcmp(dxil.values[0].bytes.Data(), "DXBC", 4) == 0 && dxil.diagnostics.IsEmpty());
    REQUIRE(spirv.status == ShaderWorkerJobStatus::Success && spirv.values.Size() == 1);
    CHECK(spirv.values[0].bytes.Size() % 4 == 0 && spirv.values[0].bytes[0] == 0x03 &&
          spirv.values[0].bytes[1] == 0x02 && spirv.values[0].bytes[2] == 0x23 &&
          spirv.values[0].bytes[3] == 0x07);
    // 컴파일 오류는 진단과 함께 CompileError, FXC 경로(D3D11_SM5)는 SH1e까지
    // UnsupportedCapability다.
    CHECK(test.result.response.jobs[2].status == ShaderWorkerJobStatus::CompileError &&
          test.result.response.jobs[2].values.IsEmpty() &&
          strstr(test.result.response.jobs[2].diagnostics.Data(), "undefined_name_c") != nullptr);
    CHECK(test.result.response.jobs[3].status == ShaderWorkerJobStatus::UnsupportedCapability);

    // 같은 입력은 같은 byte다(워커를 새로 띄워도).
    Array<uint8> first(TestAllocator());
    REQUIRE(first.TryAppend(dxil.values[0].bytes.View()));
    test.channel.Shutdown();
    ChannelCase again;
    REQUIRE(again.Make("workercompile2", false, ready) && ready);
    REQUIRE(again.Run(compile));
    REQUIRE(again.result.outcome == ShaderWorkerBatchOutcome::Completed);
    const Array<uint8>& second = again.result.response.jobs[0].values[0].bytes;
    CHECK(second.Size() == first.Size() && memcmp(second.Data(), first.Data(), first.Size()) == 0);
}

TEST(ShaderWorkerChannel_TerminatesHeldWorker) {
    ShaderWorkerChannelHooks hooks;
    hooks.held = &TerminateHeld;
    ChannelCase test;
    bool ready = false;
    if (!test.Make("workerheld", true, ready, hooks)) {
        printf(
            "[SKIP] ShaderWorkerChannel_TerminatesHeldWorker: ShaderCompileWorker is not next to "
            "the test executable\n");
        return;
    }

    REQUIRE(ready);
    RequestBuilder hold;
    REQUIRE(hold.AddJob(ShaderWorkerJobKind::Hold, "", ShaderStageCompute, ""));
    REQUIRE(test.Run(hold));
    // hook이 끝내기를 골랐다: 채널 스레드가 워커를 끝내고 그 종료를 보고한다.
    CHECK(test.result.outcome == ShaderWorkerBatchOutcome::WorkerExited);
    CHECK(test.result.exit.kind == platform::ProcessExitKind::Terminated);
    const uint32 terminated = test.result.workerProcessId;

    // 같은 채널의 다음 묶음은 새 워커가 처리한다(빈 컴파일 묶음).
    RequestBuilder empty;
    REQUIRE(test.Run(empty));
    CHECK(test.result.outcome == ShaderWorkerBatchOutcome::Completed);
    CHECK(test.result.workerProcessId != 0 && test.result.workerProcessId != terminated);
}

TEST(ShaderWorkerChannel_ReportsCrashAndAssert) {
    ChannelCase test;
    bool ready = false;
    if (!test.Make("workercrash", true, ready)) {
        printf("[SKIP] ShaderWorkerChannel_ReportsCrashAndAssert: ShaderCompileWorker is not next "
               "to the test executable\n");
        return;
    }

    REQUIRE(ready);
    for (const ShaderWorkerJobKind kind :
         {ShaderWorkerJobKind::Crash, ShaderWorkerJobKind::Assert}) {
        RequestBuilder failing;
        REQUIRE(failing.AddJob(kind, "", ShaderStageCompute, ""));
        REQUIRE(test.Run(failing));
        // 대화상자 없이 끝나야 여기에 온다(채널은 워커 종료까지 기다린다).
        CHECK(test.result.outcome == ShaderWorkerBatchOutcome::WorkerExited);
        CHECK(test.result.exit.code != 0);
        CHECK(!test.result.diagnostics.IsEmpty());
        // 다른 묶음은 성공한다.
        RequestBuilder query;
        query.request.kind = ShaderWorkerRequestKind::ToolQuery;
        REQUIRE(test.Run(query));
        CHECK(test.result.outcome == ShaderWorkerBatchOutcome::Completed);
    }
}

TEST(ShaderWorkerChannel_ParentDeathEndsWorkerAndGrandchild) {
    REQUIRE(ParentProbeRegistered);
    char worker[ChannelPathBytes];
    if (!WorkerExecutable(worker) || !test::ProcessSpawnExpected()) {
        printf(
            "[SKIP] ShaderWorkerChannel_ParentDeathEndsWorkerAndGrandchild: ShaderCompileWorker is "
            "not next to the test executable\n");
        return;
    }

    Fixture fixture;
    REQUIRE(fixture.Make("workerparent"));
    char directory[FixturePathBytes];
    REQUIRE(fixture.Child("w", directory, sizeof(directory)));
    char self[ChannelPathBytes];
    usize selfBytes = 0;
    REQUIRE(platform::GetExecutablePath(self, sizeof(self), selfBytes));
    const platform::ProcessArgument arguments[] = {{"--process-probe", 15},
                                                   {"shader-worker-parent", 20},
                                                   {worker, strlen(worker)},
                                                   {directory, strlen(directory)}};
    platform::ProcessStartInfo info;
    info.executable = self;
    info.executableBytes = selfBytes;
    info.arguments = arguments;
    info.argumentCount = sizeof(arguments) / sizeof(arguments[0]);
    info.input = platform::ProcessStreamMode::Pipe;
    info.output = platform::ProcessStreamMode::Pipe;
    info.error = platform::ProcessStreamMode::Null;
    platform::Process parent;
    REQUIRE(parent.Start(info));

    // 부모 probe가 워커·손자 PID를 알릴 때까지 읽는다(명시 gate).
    char lines[256] = {};
    usize received = 0;
    while (strstr(lines, "child ") == nullptr || strchr(strstr(lines, "child "), '\n') == nullptr) {
        const platform::FileTransfer read = parent.Read(
            platform::ProcessStream::Output, lines + received, sizeof(lines) - 1 - received);
        REQUIRE(read.status && read.bytes != 0);
        received += read.bytes;
    }

    const uint32 workerId = ReportedId(lines, "worker");
    const uint32 childId = ReportedId(lines, "child");
    REQUIRE(workerId != 0 && childId != 0);
    CHECK(platform::WaitForProcessExit(workerId, 0) == platform::WaitResult::TimedOut);
    CHECK(platform::WaitForProcessExit(childId, 0) == platform::WaitResult::TimedOut);

    // 부모가 스스로 비정상 종료한다(이 시험이 부모를 Terminate하면 job object가 자손을 함께 끝내
    // 저절로 통과한다). 워커·손자가 끝난 뒤에야 부모 Process를 닫는다.
    REQUIRE(parent.WriteInput("x", 1).status);
    REQUIRE(parent.Wait(ExitWaitMilliseconds) == platform::WaitResult::Signaled);
    platform::ProcessExit exit;
    REQUIRE(parent.GetExit(exit));
    CHECK(exit.kind == platform::ProcessExitKind::Exited && exit.code == ParentDiedExit);
    CHECK(platform::WaitForProcessExit(workerId, ExitWaitMilliseconds) ==
          platform::WaitResult::Signaled);
    CHECK(platform::WaitForProcessExit(childId, ExitWaitMilliseconds) ==
          platform::WaitResult::Signaled);
    parent.Close();
}

} // namespace shader_worker_channel_tests_file
