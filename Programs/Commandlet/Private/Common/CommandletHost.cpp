#include "CommandletHost.h"
#include "AssetAliasCommandlet.h"
#include "AssetCommandletSupport.h"
#include "AssetEditCommandlet.h"
#include "AssetIndexCommandlet.h"
#include "ContentSignCommandlet.h"
#include "LogCommandlet.h"
#include <Config/ConfigDump.h>
#include <Core/Memory/SystemAllocator.h>
#include <HostConfig/HostSettings.h>
#include <Platform/File.h>
#include <TraceAnalysis/TraceModel.h>
#include <TraceAnalysis/TraceReport.h>

#include <stdio.h>

namespace rocks::commandlet_host {
namespace {

Result<int32> CoreCheck(void*, Span<const StringView> args, CommandletOutput& output) noexcept {
    if (args.Size() != 0) {
        auto written = output.write(output.context, CommandletStream::Error,
                                    "core-check accepts no arguments.\n");
        return written ? Result<int32>::Success(2) : Result<int32>::Failure(written.Error());
    }
    SystemAllocator allocator;
    auto allocation = allocator.Allocate(64, 16, 0x434d444cu);
    if (!allocation) {
        return Result<int32>::Failure(allocation.Error());
    }
    auto released = allocator.Deallocate(allocation.Value());
    if (!released || allocator.Stats().liveAllocations != 0) {
        return Result<int32>::Failure(released ? ErrorCode::DependencyFailed : released.Error());
    }
    auto written = output.write(output.context, CommandletStream::Output,
                                "Core allocation and release: PASS\n");
    return written ? Result<int32>::Success(0) : Result<int32>::Failure(written.Error());
}

Result<int32> WriteText(CommandletOutput& output, CommandletStream stream,
                        StringView text) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(stream == CommandletStream::Error ? 1 : 0)
                   : Result<int32>::Failure(written.Error());
}

// 파일 오류를 원본 코드(FileCode·OS 오류)와 함께 알리고 1을 돌려준다.
Result<int32> FileFailure(CommandletOutput& output, const char* action, StringView path,
                          platform::FileStatus status) noexcept {
    char text[256];
    const int length = snprintf(text, sizeof(text),
                                "trace-report: cannot %s '%.*s' (file code %d, native error %u)\n",
                                action, static_cast<int>(path.ByteCount()), path.Data(),
                                static_cast<int>(status.code), status.nativeError);
    return WriteText(output, CommandletStream::Error,
                     StringView(text, length > 0 ? static_cast<usize>(length) : 0));
}

// trace-report <input.rtrace> [<output.json>]: trace 파일의 결정적 JSON report(TraceAnalysis)를
// 쓴다. 출력 경로가 없으면 표준 출력으로 낸다. 사용법 오류 2, 파일·분석 오류 1, 성공 0이다. 문맥은
// 실행 진입점이 넘긴 범용 할당자다(파일 전체와 분석 모델이 거기서 나온다).
Result<int32> TraceReport(void* context, Span<const StringView> args,
                          CommandletOutput& output) noexcept {
    if (args.Size() != 1 && args.Size() != 2) {
        auto written = output.write(output.context, CommandletStream::Error,
                                    "usage: trace-report <input.rtrace> [<output.json>]\n");
        return written ? Result<int32>::Success(2) : Result<int32>::Failure(written.Error());
    }

    // 경로는 NUL로 끝나야 하므로 복사한다(인수 view는 NUL 종료를 보장하지 않는다).
    char input[CommandletRegistry::MaxArgumentBytes + 1] = {};
    platform::MemoryCopy(input, args[0].Data(), args[0].ByteCount());
    Allocator& allocator = *static_cast<Allocator*>(context);
    Array<uint8> bytes(allocator);
    {
        platform::File file;
        if (const platform::FileStatus opened =
                file.Open(input, platform::FileOpenMode::ReadExisting);
            !opened) {
            return FileFailure(output, "open", args[0], opened);
        }

        uint64 size = 0;
        if (const platform::FileStatus sized = file.Size(size); !sized) {
            return FileFailure(output, "size", args[0], sized);
        }

        if (!bytes.TryResize(static_cast<usize>(size))) {
            return Result<int32>::Failure(ErrorCode::OutOfMemory);
        }

        const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
        if (!read.status) {
            return FileFailure(output, "read", args[0], read.status);
        }

        if (const platform::FileStatus closed = file.Close(); !closed) {
            return FileFailure(output, "close", args[0], closed);
        }
    }

    Array<uint8> json(allocator);
    {
        TraceModel model(allocator);
        if (const auto built = model.Build(bytes.View()); !built) {
            char text[160];
            const int length =
                snprintf(text, sizeof(text), "trace-report: '%.*s' is not a trace file (%s)\n",
                         static_cast<int>(args[0].ByteCount()), args[0].Data(),
                         ErrorCodeName(built.Error()));
            return WriteText(output, CommandletStream::Error,
                             StringView(text, length > 0 ? static_cast<usize>(length) : 0));
        }

        if (const auto written = WriteTraceReportJson(model, json); !written) {
            return Result<int32>::Failure(written.Error());
        }
    }

    const StringView text(reinterpret_cast<const char*>(json.Data()), json.Size());
    if (args.Size() == 1) {
        return WriteText(output, CommandletStream::Output, text);
    }

    char path[CommandletRegistry::MaxArgumentBytes + 1] = {};
    platform::MemoryCopy(path, args[1].Data(), args[1].ByteCount());
    platform::File file;
    if (const platform::FileStatus opened = file.Open(path, platform::FileOpenMode::CreateAlways);
        !opened) {
        return FileFailure(output, "create", args[1], opened);
    }

    if (const platform::FileTransfer written = file.WriteAll(json.Data(), json.Size());
        !written.status) {
        return FileFailure(output, "write", args[1], written.status);
    }

    if (const platform::FileStatus closed = file.Close(); !closed) {
        return FileFailure(output, "close", args[1], closed);
    }

    return Result<int32>::Success(0);
}

// config-dump가 보는 설정 문맥이다. 엔진 호스트와 같은 부트(HostSettings — log.*·trace.*·
// TracePresets 등록 → 엔진 층 파일 → CompleteStartup)를 거친다. 실행 파일 위에 Engine/Config가
// 없으면 엔진 층 파일 없이 코드 기본값이다. 실패는 HostSettings가 기록했다(config-dump만 빠진다).
// 모듈 설정 등록은 프로세스 로더(modules — Launch 소유)의 표를 쓴다.
bool BootSettings(HostSettings& settings, ModuleLoader& modules) noexcept {
    char engineDir[4096] = {};
    HostSettingsOptions options;
    options.modules = &modules;
    if (auto found = FindEngineDirectory(Span<char>(engineDir)); found) {
        options.engineDir = StringView(engineDir, found.Value());
    }

    return settings.Initialize(options).IsOk() && settings.LoadFiles().IsOk() &&
           settings.CompleteStartup().IsOk();
}

} // namespace

// 명령을 등록하고 실행한다. 명령의 Log 호출은 Launch가 설치한 도구 로그로 간다(명령이 로그를
// 남기지 않으면 출력은 그대로다). 설정 문맥과 명령의 작업 메모리는 allocator(실행 진입점의 범용
// 할당자)에서 나온다 — core-check만 자기 SystemAllocator를 만든다(그 명령이 점검하는 대상이다).
int Run(Span<const StringView> args, CommandletOutput output, Allocator& allocator,
        ModuleLoader& modules) noexcept {
    HostSettings settings(allocator);
    AssetCommandContext assetContext{&allocator, &modules};
    const bool settingsReady = BootSettings(settings, modules);
    CommandletRegistry registry;
    auto registered = registry.Register(
        {"core-check", "Check the Core allocator lifecycle.", &CoreCheck, nullptr});
    if (!registered) {
        return 1;
    }
    registered =
        registry.Register({"trace-report", "Write a deterministic JSON report of a .rtrace file.",
                           &TraceReport, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register(
        {"log", "Log file tools: keygen, decode, seal. Never commit or cook the private key.",
         &RunLogCommand, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register(
        {"content-sign",
         "Cooked content signing: keygen, sign, verify. Never commit or cook the key file.",
         &RunContentSignCommand, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register({"asset-list", "List the editor packages of the project.",
                                    &RunAssetListCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered = registry.Register(
        {"asset-new", "Create an editor package and save it.", &RunAssetNewCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered =
        registry.Register({"asset-set", "Assign properties of an editor package and save it.",
                           &RunAssetSetCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered = registry.Register({"asset-info", "Print the properties of an editor package.",
                                    &RunAssetInfoCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered =
        registry.Register({"asset-move", "Move or rename an editor package or a directory of them.",
                           &RunAssetMoveCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered =
        registry.Register({"asset-delete", "Delete an editor package that nothing references.",
                           &RunAssetDeleteCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered = registry.Register(
        {"asset-duplicate", "Save a copy of an editor package under a new path and AssetId.",
         &RunAssetDuplicateCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered = registry.Register({"asset-orphans-delete", "Delete an orphaned import output.",
                                    &RunAssetOrphansDeleteCommand, &assetContext});
    if (!registered) {
        return 1;
    }
    registered = registry.Register(
        {"asset-index-rebuild",
         "Rescan the project without the index cache, clean temporary files and rewrite the cache.",
         &RunAssetIndexRebuildCommand, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register({"asset-index-verify",
                                    "Check the editor packages of the project and report problems.",
                                    &RunAssetIndexVerifyCommand, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register({"asset-orphans-list", "List the orphaned import outputs.",
                                    &RunAssetOrphansListCommand, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register({"asset-alias-list", "List the path aliases of the project.",
                                    &RunAssetAliasListCommand, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register({"asset-alias-remove", "Remove a path alias of the project.",
                                    &RunAssetAliasRemoveCommand, &allocator});
    if (!registered) {
        return 1;
    }
    registered = registry.Register(
        {"asset-alias-prune", "Remove the expired path aliases that no config file still names.",
         &RunAssetAliasPruneCommand, &allocator});
    if (!registered) {
        return 1;
    }
    if (settingsReady) {
        registered = config::RegisterConfigDumpCommandlet(registry, settings.Context());
        if (!registered) {
            return 1;
        }
    }
    auto result = registry.Run(args, output);
    if (!result) {
        if (output.write) {
            // 진단 출력도 실패할 수 있다. 복구할 수 없으면 실패 코드를 그대로 반환한다.
            (void)output.write(output.context, CommandletStream::Error,
                               "Commandlet execution or output failed.\n");
        }
        return 1;
    }
    return static_cast<int>(result.Value());
}

} // namespace rocks::commandlet_host
