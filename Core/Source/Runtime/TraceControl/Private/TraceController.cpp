#include <TraceControl/TraceController.h>

#include <Base/NumericLimits.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/StringBuilder.h>
#include <TraceControl/TraceControl.module.generated.h>

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

// 콘솔 출력에 한 줄을 쓴다(쓰기 실패는 명령 결과를 바꾸지 않는다).
void WriteLine(config::ConsoleOutput& output, Allocator& allocator, StringView text) noexcept {
    if (output.write == nullptr) {
        return;
    }

    StringBuilder<128> line(allocator);
    if (line.TryAppend(text) && line.TryAppend("\n")) {
        static_cast<void>(output.write(output.context, line.View()));
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

// 콘솔 명령 함수들이다. context는 TraceController다.
Result<void> RunEnable(void* context, const void* arguments,
                       config::ConsoleOutput& output) noexcept;
Result<void> RunDisable(void* context, const void* arguments,
                        config::ConsoleOutput& output) noexcept;
Result<void> RunFile(void* context, const void* arguments, config::ConsoleOutput& output) noexcept;
Result<void> RunStop(void* context, const void* arguments, config::ConsoleOutput& output) noexcept;
Result<void> RunSnapshot(void* context, const void* arguments,
                         config::ConsoleOutput& output) noexcept;

} // namespace

// 콘솔 명령이 제어기의 비공개 동작과 문맥을 쓰는 통로다.
struct TraceControllerAccess {
    static Allocator& AllocatorOf(TraceController& controller) noexcept {
        return controller.allocator_;
    }
};

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

    return config.RegisterSettings<TraceCaptureSettings>();
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
    if (file_.value != 0 && session_.IsRunning()) {
        static_cast<void>(session_.DetachOutput(file_));
    }
}

Result<void> TraceController::RegisterConsoleCommands() noexcept {
    config::ConsoleCommandDefinition enable;
    enable.name = "trace.enable";
    enable.help = "Enable trace channels or presets: trace.enable channels=cpu,gpu";
    enable.execute = &RunEnable;
    enable.context = this;
    if (auto registered = config_.RegisterConsoleCommand<TraceChannelArguments>(enable);
        !registered) {
        return registered;
    }

    config::ConsoleCommandDefinition disable;
    disable.name = "trace.disable";
    disable.help = "Disable trace channels or presets: trace.disable channels=log";
    disable.execute = &RunDisable;
    disable.context = this;
    if (auto registered = config_.RegisterConsoleCommand<TraceChannelArguments>(disable);
        !registered) {
        return registered;
    }

    config::ConsoleCommandDefinition file;
    file.name = "trace.file";
    file.help = "Start writing the trace to a file: trace.file path=Saved/Traces/run.rtrace";
    file.execute = &RunFile;
    file.context = this;
    if (auto registered = config_.RegisterConsoleCommand<TracePathArguments>(file); !registered) {
        return registered;
    }

    config::ConsoleCommandDefinition stop;
    stop.name = "trace.stop";
    stop.help = "Stop the file output started by trace.file";
    stop.execute = &RunStop;
    stop.context = this;
    if (auto registered = config_.RegisterConsoleCommand(stop); !registered) {
        return registered;
    }

    config::ConsoleCommandDefinition snapshot;
    snapshot.name = "trace.snapshot";
    snapshot.help =
        "Write the recent trace ring to a file: trace.snapshot path=Saved/Traces/recent.rtrace";
    snapshot.execute = &RunSnapshot;
    snapshot.context = this;
    return config_.RegisterConsoleCommand<TracePathArguments>(snapshot);
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

    const auto timeout = static_cast<uint32>(ClampUnsigned(
        config_.Get<TraceCaptureSettings>().snapshotLockTimeoutMs, NumericLimits<uint32>::Max()));
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

Result<void> RunChannels(void* context, const void* arguments, config::ConsoleOutput& output,
                         bool enabled) noexcept {
    auto& controller = *static_cast<TraceController*>(context);
    const auto& args = *static_cast<const TraceChannelArguments*>(arguments);
    auto change = controller.SetChannels(args.channels.View(), enabled);
    if (!change) {
        return Result<void>::Failure(change.Error());
    }

    Allocator& allocator = TraceControllerAccess::AllocatorOf(controller);
    StringBuilder<128> line(allocator);
    static_cast<void>(line.TryAppendFormat("{} channel(s) {}, {} unknown, {} read-only",
                                           change.Value().applied, enabled ? "enabled" : "disabled",
                                           change.Value().unknown, change.Value().readOnly));
    WriteLine(output, allocator, line.View());
    return Result<void>::Success();
}

Result<void> RunEnable(void* context, const void* arguments,
                       config::ConsoleOutput& output) noexcept {
    return RunChannels(context, arguments, output, true);
}

Result<void> RunDisable(void* context, const void* arguments,
                        config::ConsoleOutput& output) noexcept {
    return RunChannels(context, arguments, output, false);
}

Result<void> RunFile(void* context, const void* arguments, config::ConsoleOutput& output) noexcept {
    auto& controller = *static_cast<TraceController*>(context);
    const auto& args = *static_cast<const TracePathArguments*>(arguments);
    if (auto opened = controller.OpenFile(args.path.View()); !opened) {
        return opened;
    }

    WriteLine(output, TraceControllerAccess::AllocatorOf(controller), "Trace file output started");
    return Result<void>::Success();
}

Result<void> RunStop(void* context, const void* arguments, config::ConsoleOutput& output) noexcept {
    (void)arguments;
    auto& controller = *static_cast<TraceController*>(context);
    if (auto closed = controller.CloseFile(); !closed) {
        return closed;
    }

    WriteLine(output, TraceControllerAccess::AllocatorOf(controller), "Trace file output stopped");
    return Result<void>::Success();
}

Result<void> RunSnapshot(void* context, const void* arguments,
                         config::ConsoleOutput& output) noexcept {
    auto& controller = *static_cast<TraceController*>(context);
    const auto& args = *static_cast<const TracePathArguments*>(arguments);
    auto written = controller.WriteSnapshot(args.path.View());
    if (!written) {
        return Result<void>::Failure(written.Error());
    }

    Allocator& allocator = TraceControllerAccess::AllocatorOf(controller);
    StringBuilder<128> line(allocator);
    static_cast<void>(line.TryAppendFormat("Trace snapshot written ({} bytes, {})",
                                           written.Value().bytes,
                                           written.Value().complete ? "complete" : "incomplete"));
    WriteLine(output, allocator, line.View());
    return Result<void>::Success();
}

} // namespace

} // namespace rocks::trace_control
