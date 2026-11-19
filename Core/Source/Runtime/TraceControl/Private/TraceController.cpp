#include <TraceControl/TraceController.h>

#include <Base/NumericLimits.h>
#include <Config/CVar.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/StringBuilder.h>
#include <TraceControl/TraceControl.module.generated.h>
#include <TraceControl/TraceSettings.generated.h>

namespace rocks::trace_control {

namespace {

// trace 제어(명령줄·콘솔 명령)의 로그 카테고리다.
inline constexpr LogCategory LogTraceControl{"TraceControl"};

// 이름 하나가 가리키는 묶음이다(TracePresetSettings의 필드). 묶음을 더하면 여기에도 더한다.
struct PresetEntry {
    StringView name;
    Array<String> TracePresetSettings::* channels;
};

constexpr PresetEntry Presets[] = {
    {"default", &TracePresetSettings::defaultChannels},
};

const Array<String>* FindPreset(const TracePresetSettings& settings, StringView name) noexcept {
    for (const PresetEntry& entry : Presets) {
        if (entry.name == name) {
            return &(settings.*entry.channels);
        }
    }

    return nullptr;
}

constexpr StringView TraceArgumentPrefix = "--trace=";

// 콘솔에 한 줄을 쓴다(쓰기 실패는 명령 결과를 바꾸지 않는다).
void WriteLine(cvar::CommandContext& context, StringView text) noexcept {
    StringBuilder<128> line(context.GetAllocator());
    if (line.TryAppend(text) && line.TryAppend("\n")) {
        static_cast<void>(context.Write(line.View()));
    }
}

template <class Value>
uint64 ClampUnsigned(Value value, uint64 maximum) noexcept {
    if (value <= 0) {
        return 0;
    }

    const auto unsignedValue = static_cast<uint64>(value);
    return unsignedValue < maximum ? unsignedValue : maximum;
}

// 콘솔 명령 함수들이다(대상은 BindCommand로 묶은 TraceController — CVar 15절).
Result<void> RunEnable(cvar::CommandContext& context, StringView channels) noexcept;
Result<void> RunDisable(cvar::CommandContext& context, StringView channels) noexcept;
Result<void> RunFile(cvar::CommandContext& context, StringView path) noexcept;
Result<void> RunStop(cvar::CommandContext& context) noexcept;
Result<void> RunSnapshot(cvar::CommandContext& context, StringView path) noexcept;

} // namespace

// trace 콘솔 명령이다(인자는 줄의 나머지 — 쉼표로 나눈 채널·묶음 이름, 파일 경로).
constinit cvar::ConsoleCommand TraceEnableCommand =
    cvar::Command<&RunEnable>("trace.enable")
        .Help("Enable trace channels or presets: trace.enable cpu,gpu");

constinit cvar::ConsoleCommand TraceDisableCommand =
    cvar::Command<&RunDisable>("trace.disable")
        .Help("Disable trace channels or presets: trace.disable log");

constinit cvar::ConsoleCommand TraceFileCommand =
    cvar::Command<&RunFile>("trace.file")
        .Help("Start writing the trace to a file: trace.file Saved/Traces/run.rtrace");

constinit cvar::ConsoleCommand TraceStopCommand =
    cvar::Command<&RunStop>("trace.stop").Help("Stop the file output started by trace.file");

constinit cvar::ConsoleCommand TraceSnapshotCommand =
    cvar::Command<&RunSnapshot>("trace.snapshot")
        .Help("Write the recent trace ring to a file: trace.snapshot Saved/Traces/recent.rtrace");

constinit BootCVar<int64> TracePoolMaxBytesVar =
    cvar::Boot("trace.pool.max_bytes", int64{64} * 1024 * 1024)
        .Help("Trace block pool limit in bytes (rounded down to whole pages). Fixed after startup.")
        .Range(0, NumericLimits<int64>::Max());

constinit BootCVar<int32> TracePoolWarningPercentVar =
    cvar::Boot("trace.pool.warn_percent", 80)
        .Help("Trace block pool usage warning in percent (0 turns it off). Fixed after startup.")
        .Range(0, MaxTracePoolWarningPercent);

constinit BootCVar<int64> TraceRecentBytesVar =
    cvar::Boot("trace.recent.bytes", int64{4} * 1024 * 1024)
        .Help("Size of the recent trace record ring in bytes (0 means no ring; trace.snapshot "
              "writes it). Fixed after startup.")
        .Range(0, NumericLimits<int64>::Max());

constinit BootCVar<int32> TraceWriterPeriodVar =
    cvar::Boot("trace.writer.period_ms", 16)
        .Help("Flush period of the trace writer thread in milliseconds. Fixed after startup.")
        .Range(0, NumericLimits<int32>::Max());

constinit BootCVar<int32> TraceStatsIntervalVar =
    cvar::Boot("trace.stats.interval_pumps", 60)
        .Help("Pumps between capture statistics records of the stats channel. Fixed after "
              "startup.")
        .Range(0, NumericLimits<int32>::Max());

constinit CVar<int32> TraceSnapshotLockTimeoutVar =
    cvar::Var("trace.snapshot.lock_timeout_ms", 1000)
        .Help("How long trace.snapshot waits for the trace writer lock, in milliseconds.")
        .Range(0, NumericLimits<int32>::Max());

Result<void> RegisterTraceControlTypes(ModuleRegistration& registration) noexcept {
    return generated::TraceControl::RegisterModuleTypes(registration);
}

Result<void> RegisterTraceSettings(config::ConfigContext& config) noexcept {
    if (auto region = config.RegisterConsoleRegion("trace"); !region) {
        return region;
    }

    if (auto presets = config.RegisterSettings<TracePresetSettings>(); !presets) {
        return presets;
    }

    return config.Register(TracePoolMaxBytesVar, TracePoolWarningPercentVar, TraceRecentBytesVar,
                           TraceWriterPeriodVar, TraceStatsIntervalVar,
                           TraceSnapshotLockTimeoutVar);
}

Result<TraceCaptureSettings>
ReadTraceCaptureSettings(const config::ConfigContext& config) noexcept {
    SharedPtr<const config::ConfigSnapshot> snapshot = config.Acquire();
    if (snapshot.Get() == nullptr || !snapshot->IsSealed()) {
        return Result<TraceCaptureSettings>::Failure(ErrorCode::InvalidArgument);
    }

    cvar::SnapshotBinding bound(Move(snapshot));
    TraceCaptureSettings settings;
    settings.poolMaxBytes = TracePoolMaxBytesVar.Get();
    settings.poolWarningPercent = TracePoolWarningPercentVar.Get();
    settings.recentBytes = TraceRecentBytesVar.Get();
    settings.writerPeriodMs = TraceWriterPeriodVar.Get();
    settings.statsIntervalPumps = TraceStatsIntervalVar.Get();
    settings.snapshotLockTimeoutMs = TraceSnapshotLockTimeoutVar.Get();
    return Result<TraceCaptureSettings>::Success(settings);
}

TraceSessionSettings MakeTraceSessionSettings(const TraceCaptureSettings& settings,
                                              Allocator& allocator) noexcept {
    TraceSessionSettings session;
    session.allocator = &allocator;
    session.poolMaxBytes =
        static_cast<usize>(ClampUnsigned(settings.poolMaxBytes, NumericLimits<usize>::Max()));
    session.poolWarningPercent = static_cast<uint32>(
        ClampUnsigned(settings.poolWarningPercent, NumericLimits<uint32>::Max()));
    session.recentBytes =
        static_cast<usize>(ClampUnsigned(settings.recentBytes, NumericLimits<usize>::Max()));
    session.writerPeriodMs =
        static_cast<uint32>(ClampUnsigned(settings.writerPeriodMs, NumericLimits<uint32>::Max()));
    session.statsIntervalPumps = static_cast<uint32>(
        ClampUnsigned(settings.statsIntervalPumps, NumericLimits<uint32>::Max()));
    return session;
}

TraceController::TraceController(Allocator& allocator, TraceSession& session,
                                 config::ConfigContext& config) noexcept
    : allocator_(allocator), session_(session), config_(config) {
}

TraceController::~TraceController() noexcept {
    // 명령 묶음을 푼다(등록하지 않았으면 NotFound — 무시한다).
    const cvar::ConsoleCommand* const commands[] = {&TraceEnableCommand, &TraceDisableCommand,
                                                    &TraceFileCommand, &TraceStopCommand,
                                                    &TraceSnapshotCommand};
    for (const cvar::ConsoleCommand* command : commands) {
        static_cast<void>(config_.UnbindCommand(*command, *this));
    }

    if (file_.value != 0 && session_.IsRunning()) {
        static_cast<void>(session_.DetachOutput(file_));
    }
}

Result<void> TraceController::RegisterConsoleCommands() noexcept {
    if (auto registered =
            config_.Register(TraceEnableCommand, TraceDisableCommand, TraceFileCommand,
                             TraceStopCommand, TraceSnapshotCommand);
        !registered) {
        return registered;
    }

    const cvar::ConsoleCommand* const commands[] = {&TraceEnableCommand, &TraceDisableCommand,
                                                    &TraceFileCommand, &TraceStopCommand,
                                                    &TraceSnapshotCommand};
    for (const cvar::ConsoleCommand* command : commands) {
        if (auto bound = config_.BindCommand(*command, *this); !bound) {
            return bound;
        }
    }

    return Result<void>::Success();
}

Result<void> TraceController::SetChannel(StringView name, bool enabled, bool fromPreset,
                                         TraceChannelChange& change) noexcept {
    const Result<void> set = session_.SetChannelEnabled(name, enabled);
    if (set) {
        ++change.applied;
        return Result<void>::Success();
    }

    switch (set.Error()) {
    case ErrorCode::NotFound:
        ++change.unknown;
        // 묶음의 이름은 아직 등록되지 않은 모듈의 채널일 수 있다(기능이 꺼진 빌드 등).
        if (!fromPreset) {
            Log::Warning(LogTraceControl, "Unknown trace channel or preset '{Name}' was ignored",
                         name);
        }

        return Result<void>::Success();
    case ErrorCode::Unsupported:
        ++change.readOnly;
        Log::Warning(LogTraceControl, "Trace channel '{Name}' is read-only and was not changed",
                     name);
        return Result<void>::Success();
    default:
        return Result<void>::Failure(set.Error());
    }
}

Result<void> TraceController::ApplyName(StringView name, bool enabled,
                                        TraceChannelChange& change) noexcept {
    const Array<String>* preset = FindPreset(config_.Get<TracePresetSettings>(), name);
    if (preset == nullptr) {
        return SetChannel(name, enabled, false, change);
    }

    for (const String& channel : *preset) {
        if (auto set = SetChannel(channel.View(), enabled, true, change); !set) {
            return set;
        }
    }

    return Result<void>::Success();
}

Result<TraceChannelChange> TraceController::SetChannels(StringView list, bool enabled) noexcept {
    if (!session_.IsRunning()) {
        return Result<TraceChannelChange>::Failure(ErrorCode::WrongContext);
    }

    TraceChannelChange change;
    StringView rest = list;
    for (;;) {
        StringView item = rest;
        StringView next;
        const bool more = rest.Split(",", item, next, StringComparison::CaseSensitive);
        item = item.TrimAscii();
        if (!item.IsEmpty()) {
            bool itemEnabled = enabled;
            if (item.StartsWith("-", StringComparison::CaseSensitive)) {
                itemEnabled = !enabled;
                item = item.TryRightBytes(item.ByteCount() - 1).Value().TrimAscii();
            }

            if (auto applied = ApplyName(item, itemEnabled, change); !applied) {
                return Result<TraceChannelChange>::Failure(applied.Error());
            }
        }

        if (!more) {
            break;
        }

        rest = next;
    }

    return Result<TraceChannelChange>::Success(change);
}

Result<TraceChannelChange>
TraceController::ApplyCommandLine(Span<const StringView> arguments) noexcept {
    TraceChannelChange total;
    for (const StringView argument : arguments) {
        if (!argument.StartsWith(TraceArgumentPrefix, StringComparison::CaseSensitive)) {
            continue;
        }

        auto list = argument.TryRightBytes(argument.ByteCount() - TraceArgumentPrefix.ByteCount());
        if (!list) {
            return Result<TraceChannelChange>::Failure(list.Error());
        }

        auto change = SetChannels(list.Value(), true);
        if (!change) {
            return change;
        }

        total.applied += change.Value().applied;
        total.unknown += change.Value().unknown;
        total.readOnly += change.Value().readOnly;
    }

    return Result<TraceChannelChange>::Success(total);
}

Result<void> TraceController::OpenFile(StringView path) noexcept {
    if (path.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    String terminated(allocator_);
    if (auto assigned = terminated.TryAssign(path); !assigned) {
        return assigned;
    }

    if (file_.value != 0) {
        if (auto closed = CloseFile(); !closed) {
            return closed;
        }
    }

    auto attached = session_.AttachFileOutput(terminated.CStr());
    if (!attached) {
        const platform::FileStatus status = session_.Stats().lastOutputFileStatus;
        Log::Warning(LogTraceControl,
                     "Could not open trace file '{Path}' ({Error}, file code {FileCode}, OS error "
                     "{OsError})",
                     path, ErrorCodeName(attached.Error()), static_cast<uint32>(status.code),
                     status.nativeError);
        return Result<void>::Failure(attached.Error());
    }

    file_ = attached.Value();
    return Result<void>::Success();
}

Result<void> TraceController::CloseFile() noexcept {
    if (file_.value == 0) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const TraceOutputId file = file_;
    file_ = TraceOutputId{};
    auto detached = session_.DetachOutput(file);
    if (!detached && detached.Error() != ErrorCode::NotFound) {
        const platform::FileStatus status = session_.Stats().lastOutputFileStatus;
        Log::Warning(LogTraceControl,
                     "Closing the trace file failed ({Error}, file code {FileCode}, OS error "
                     "{OsError})",
                     ErrorCodeName(detached.Error()), static_cast<uint32>(status.code),
                     status.nativeError);
    }

    return detached;
}

Result<TraceSnapshotInfo> TraceController::WriteSnapshot(StringView path) noexcept {
    if (path.IsEmpty()) {
        return Result<TraceSnapshotInfo>::Failure(ErrorCode::InvalidArgument);
    }

    String terminated(allocator_);
    if (auto assigned = terminated.TryAssign(path); !assigned) {
        return Result<TraceSnapshotInfo>::Failure(assigned.Error());
    }

    // 명령은 Sync 5단계에 게임 lane에서 돈다(이 문맥의 snapshot이 묶여 있다 — CVar 7.1절).
    const auto timeout = static_cast<uint32>(
        ClampUnsigned(TraceSnapshotLockTimeoutVar.Get(), NumericLimits<uint32>::Max()));
    auto written = session_.WriteSnapshot(terminated.CStr(), timeout);
    if (!written) {
        const platform::FileStatus status = session_.Stats().lastOutputFileStatus;
        Log::Warning(LogTraceControl,
                     "Could not write trace snapshot '{Path}' ({Error}, file code {FileCode}, OS "
                     "error {OsError})",
                     path, ErrorCodeName(written.Error()), static_cast<uint32>(status.code),
                     status.nativeError);
        return written;
    }

    if (!written.Value().complete) {
        Log::Warning(LogTraceControl,
                     "Trace snapshot '{Path}' is incomplete: the trace writer lock was not "
                     "acquired within {TimeoutMs} ms",
                     path, timeout);
    }

    return written;
}

namespace {

Result<void> RunChannels(cvar::CommandContext& context, StringView channels,
                         bool enabled) noexcept {
    TraceController* controller = context.Target<TraceController>();
    if (controller == nullptr) {
        WriteLine(context, "no trace controller");
        return Result<void>::Success();
    }

    auto change = controller->SetChannels(channels.TrimAscii(), enabled);
    if (!change) {
        return Result<void>::Failure(change.Error());
    }

    StringBuilder<128> line(context.GetAllocator());
    static_cast<void>(line.TryAppendFormat("{} channel(s) {}, {} unknown, {} read-only",
                                           change.Value().applied, enabled ? "enabled" : "disabled",
                                           change.Value().unknown, change.Value().readOnly));
    WriteLine(context, line.View());
    return Result<void>::Success();
}

Result<void> RunEnable(cvar::CommandContext& context, StringView channels) noexcept {
    return RunChannels(context, channels, true);
}

Result<void> RunDisable(cvar::CommandContext& context, StringView channels) noexcept {
    return RunChannels(context, channels, false);
}

Result<void> RunFile(cvar::CommandContext& context, StringView path) noexcept {
    TraceController* controller = context.Target<TraceController>();
    if (controller == nullptr) {
        WriteLine(context, "no trace controller");
        return Result<void>::Success();
    }

    if (auto opened = controller->OpenFile(path.TrimAscii()); !opened) {
        return opened;
    }

    WriteLine(context, "Trace file output started");
    return Result<void>::Success();
}

Result<void> RunStop(cvar::CommandContext& context) noexcept {
    TraceController* controller = context.Target<TraceController>();
    if (controller == nullptr) {
        WriteLine(context, "no trace controller");
        return Result<void>::Success();
    }

    if (auto closed = controller->CloseFile(); !closed) {
        return closed;
    }

    WriteLine(context, "Trace file output stopped");
    return Result<void>::Success();
}

Result<void> RunSnapshot(cvar::CommandContext& context, StringView path) noexcept {
    TraceController* controller = context.Target<TraceController>();
    if (controller == nullptr) {
        WriteLine(context, "no trace controller");
        return Result<void>::Success();
    }

    auto written = controller->WriteSnapshot(path.TrimAscii());
    if (!written) {
        return Result<void>::Failure(written.Error());
    }

    StringBuilder<128> line(context.GetAllocator());
    static_cast<void>(line.TryAppendFormat("Trace snapshot written ({} bytes, {})",
                                           written.Value().bytes,
                                           written.Value().complete ? "complete" : "incomplete"));
    WriteLine(context, line.View());
    return Result<void>::Success();
}

} // namespace

} // namespace rocks::trace_control
