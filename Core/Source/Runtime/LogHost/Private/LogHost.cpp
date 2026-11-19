#include <LogHost/LogHost.h>

#include <Compression/FixedWorkspaceCompressor.h>
#include <Core/Diagnostics/Log.h>
#include <Crypto/Bytes.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

// 파일 크기 상한의 범위다(64 KiB – 1 TiB).
inline constexpr int64 MinLogFileMaxBytes = int64{64} * 1024;
inline constexpr int64 MaxLogFileMaxBytes = int64{1024} * 1024 * 1024 * 1024;

// log.file.max_bytes·log.file.retention의 기본값이다(FileLogSinkConfig 기본값과 같다).
inline constexpr int64 DefaultLogFileMaxBytes =
    static_cast<int64>(FileLogSinkConfig::DefaultMaxBytes);
inline constexpr int32 DefaultLogFileRetention =
    static_cast<int32>(FileLogSinkConfig::DefaultRetention);

constinit BootCVar<LogHostMode> LogModeVar =
    cvar::Boot("log.mode", LogHostMode::Default)
        .Help("Log recording mode: default (the host preset), async (writer thread; engine hosts) "
              "or sync (the calling thread renders and writes; tools). Sync on an engine host "
              "makes the main tick wait for every sink. Fixed after startup.");

constinit CVar<LogLevel> LogLevelVar =
    cvar::Var("log.level", LogLevel::Info)
        .Help("Global minimum log level: trace, debug, info, warning, error or fatal. Levels below "
              "the compiled minimum stay compiled out. Applied at the next config sync.");

constinit CVar<String> LogLevelsVar =
    cvar::Var("log.levels", "")
        .Help("Per-category log levels as comma-separated prefix=level pairs, for example "
              "\"Graphics=warning,Engine.World=debug\". Prefixes match dotted category names "
              "case-insensitively (longest prefix wins). Applied at the next config sync.")
        .Validate(CheckLogLevelsSyntax);

constinit CVar<String> LogRateLimitsVar =
    cvar::Var("log.rate_limits", "")
        .Help("Per-category rate limits as comma-separated prefix=records-per-second pairs, for "
              "example \"Graphics=20,Engine.World.Tick=100\" (at most 16 entries, 1 to 1000000 "
              "each). Trace to Warning records above the limit in a one-second window are dropped "
              "and counted; Error and Fatal are never limited. Applied at the next config sync.")
        .Validate(CheckLogRateLimitsSyntax);

constinit BootCVar<String> LogConsoleLayoutVar =
    cvar::Boot("log.console.layout", "")
        .Help("Layout pattern of the console (stderr) log, for example \"[{Level:u3}] "
              "{Message}{NewLine}\". Empty means the default console layout. Fixed after "
              "startup.")
        .Validate(CheckLogLayoutSyntax);

constinit BootCVar<bool> LogFileEnabledVar =
    cvar::Boot("log.file.enabled", false).Help("Write log records to files. Fixed after startup.");

constinit BootCVar<String> LogFileDirectoryVar =
    cvar::Boot("log.file.directory", "Saved/Logs")
        .Help("Folder for log files (<host>-<UTC start>.log). Fixed after startup.");

constinit BootCVar<LogFileFormat> LogFileFormatVar =
    cvar::Boot("log.file.format", LogFileFormat::Text)
        .Help("Log file contents: text (the file layout) or jsonlines. Fixed after startup.");

constinit BootCVar<String> LogFileLayoutVar =
    cvar::Boot("log.file.layout", "")
        .Help("Layout pattern of text log files. Empty means the default console layout. Ignored "
              "when log.file.format is jsonlines. Fixed after startup.")
        .Validate(CheckLogLayoutSyntax);

constinit BootCVar<int64> LogFileMaxBytesVar =
    cvar::Boot("log.file.max_bytes", DefaultLogFileMaxBytes)
        .Help("Size limit of one log file in bytes; a new file starts when the next batch would "
              "pass it. Fixed after startup.")
        .Range(MinLogFileMaxBytes, MaxLogFileMaxBytes);

constinit BootCVar<int32> LogFileRetentionVar =
    cvar::Boot("log.file.retention", DefaultLogFileRetention)
        .Help("Number of this host's log files kept in the folder, including the current one; "
              "the oldest are deleted. Fixed after startup.")
        .Range(1, static_cast<int32>(MaxLogFileRetention));

constinit BootCVar<int32> LogFileMaxAgeVar =
    cvar::Boot("log.file.max_age_minutes", 0)
        .Help("Start a new log file when a batch's first record is this many minutes after the "
              "file's first record (elapsed monotonic time, not wall-clock days). 0 turns it off. "
              "Fixed after startup.")
        .Range(0, MaxLogFileAgeMinutes);

constinit BootCVar<bool> LogFileCompressVar =
    cvar::Boot("log.file.compress", false)
        .Help("Compress each flushed batch of the log file as an independent zstd frame "
              "(*.log.zst, readable with zstd -d; with log.file.encrypt the frames are sealed "
              "after compression). Fixed after startup.");

constinit BootCVar<bool> LogFileEncryptVar =
    cvar::Boot("log.file.encrypt", false)
        .Help("Seal log files to the developer public key (log.file.recipient_key). If the key is "
              "missing or invalid no log file is written; there is never a plaintext fallback. "
              "Fixed after startup.");

constinit BootCVar<String> LogFileRecipientKeyVar =
    cvar::Boot("log.file.recipient_key", "")
        .Help("Developer X-Wing public key for log.file.encrypt, base64 (RocksCommandlet log "
              "keygen). It is not a secret. The private key never goes into the repository or "
              "cooked output. Fixed after startup.");

namespace {

// 이 모듈이 남기는 진단의 카테고리다("Log" 아래).
constexpr LogCategory LogHostCategory{"Log.Host"};

// 레이아웃 설정 하나를 해석한다(묶인 snapshot의 값). 비어 있으면 기본 콘솔 레이아웃이다. 문법은
// 설정 단계에서 Validate(CheckLogLayoutSyntax — Parse와 같은 검사)가 거부하므로 여기서 실패하는
// 일은 없다. 방어로 Error를 한 번 남기고 기본 레이아웃으로 계속한다(엔진은 멈추지 않는다, P3).
LogLayout ResolveLayout(StringView name, StringView pattern) noexcept {
    if (pattern.IsEmpty()) {
        return LogLayout::DefaultConsole();
    }

    LogLayoutParseError error;
    auto parsed = LogLayout::Parse(pattern, {}, &error);
    if (!parsed) {
        Log::Error(LogHostCategory,
                   "{Setting} is not a valid layout at byte {Offset}: {Reason}; using the default "
                   "layout",
                   name, error.offset, error.reason);
        return LogLayout::DefaultConsole();
    }

    return parsed.Value();
}

// "prefix=level,prefix=level" 목록을 항목마다 visit(entry)로 넘긴다(빈 항목은 건너뛴다).
template <class Visit>
void ForEachEntry(StringView list, Visit&& visit) noexcept {
    usize start = 0;
    while (start <= list.ByteCount()) {
        usize end = start;
        while (end < list.ByteCount() && list.Data()[end] != ',') {
            ++end;
        }

        const StringView entry =
            detail::TrimLogSettingText(StringView(list.Data() + start, end - start));
        if (!entry.IsEmpty()) {
            visit(entry);
        }

        start = end + 1;
    }
}

// 항목 하나를 접두사와 레벨로 나눈다(상수 평가 검사 CheckLogLevelsSyntax와 같은 나누기).
bool SplitEntry(StringView entry, StringView& prefix, LogLevel& level) noexcept {
    StringView name;
    return detail::SplitLogSettingEntry(entry, prefix, name) && !prefix.IsEmpty() &&
           TryParseLogLevel(name, level);
}

} // namespace

struct LogHost::AbandonedOutputs {
    UniquePtr<LogSystem> logs;
    UniquePtr<StderrLogSink> console;
    UniquePtr<FileLogSink> file;
    UniquePtr<crypto::LogFileSealer> sealer;
    UniquePtr<LogBatchCompressor> compressor;
};

namespace {

// 파일 묶음을 독립 zstd frame으로 압축한다(Compression 고정 작업 공간 문맥, 만든 뒤 무할당).
class ZstdLogBatchCompressor final : public LogBatchCompressor {
public:
    explicit ZstdLogBatchCompressor(FixedWorkspaceCompressor&& compressor) noexcept
        : compressor_(Move(compressor)) {
    }

    usize Bound(usize batchBytes) const noexcept override {
        return FixedWorkspaceCompressor::Bound(batchBytes);
    }

    Result<usize> TryCompress(Span<const uint8> batch, Span<uint8> out) noexcept override {
        return compressor_.TryCompress(batch, out);
    }

private:
    FixedWorkspaceCompressor compressor_;
};

} // namespace

bool TryParseLogLevel(StringView text, LogLevel& level) noexcept {
    for (usize index = 0; index < LogLevelCount; ++index) {
        if (EqualsIgnoringAsciiCase(text, detail::LogLevelSettingNames[index])) {
            level = static_cast<LogLevel>(index);
            return true;
        }
    }

    return false;
}

Result<void> RegisterLogSettings(config::ConfigContext& context) noexcept {
    return context.Register(LogModeVar, LogLevelVar, LogLevelsVar, LogRateLimitsVar,
                            LogConsoleLayoutVar, LogFileEnabledVar, LogFileDirectoryVar,
                            LogFileFormatVar, LogFileLayoutVar, LogFileMaxBytesVar,
                            LogFileRetentionVar, LogFileMaxAgeVar, LogFileCompressVar,
                            LogFileEncryptVar, LogFileRecipientKeyVar);
}

LogHost::LogHost(Allocator& allocator) noexcept : allocator_(allocator) {
}

LogHost::~LogHost() noexcept {
    Stop();
}

Result<void> LogHost::Start(const LogSystemConfig& preset, const config::ConfigContext* settings,
                            StringView hostName) noexcept {
    if (logs_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (writerAbandoned_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (settings == nullptr) {
        return StartWith(preset, false, hostName);
    }

    // 설정 문맥의 최신 snapshot을 묶어 핸들로 읽는다. 부팅 값을 읽으므로 봉인 뒤여야 한다.
    SharedPtr<const config::ConfigSnapshot> snapshot = settings->Acquire();
    if (snapshot.Get() == nullptr || !snapshot->IsSealed()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    cvar::SnapshotBinding bound(Move(snapshot));
    return StartWith(preset, true, hostName);
}

Result<void> LogHost::StartWith(const LogSystemConfig& preset, bool settings,
                                StringView hostName) noexcept {
    LogSystemConfig config = preset;
    if (settings) {
        const LogHostMode mode = LogModeVar.Get();
        if (mode == LogHostMode::Async) {
            config.mode = LogMode::Async;
        } else if (mode == LogHostMode::Sync) {
            config.mode = LogMode::Sync;
        }

        // log.level은 늘 값이 있으므로(기본 info) 프리셋의 globalMinimum을 덮어쓴다.
        config.globalMinimum = LogLevelVar.Get();
    }

    auto console = UniquePtr<StderrLogSink>::TryMake(
        allocator_, settings ? ResolveLayout(LogConsoleLayoutVar.Name(), LogConsoleLayoutVar.Get())
                             : LogLayout::DefaultConsole());
    if (!console) {
        return Result<void>::Failure(console.Error());
    }

    auto made = UniquePtr<LogSystem>::TryMake(allocator_, allocator_, config);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    auto reserved = allocator_.Allocate(sizeof(AbandonedOutputs), alignof(AbandonedOutputs));
    if (!reserved) {
        return Result<void>::Failure(reserved.Error());
    }

    abandonedStorage_ = reserved.Value();
    stderr_ = Move(console.Value());
    logs_ = Move(made.Value());
    fileError_ = ErrorCode::None;
    if (auto added = logs_->AddSink(*stderr_); !added) {
        Stop();
        return added;
    }

    if (settings) {
        PrepareFileSink(hostName);
    }

    if (auto started = logs_->Start(); !started) {
        Stop();
        return started;
    }

    if (settings) {
        // Watch의 기준점은 Start가 읽은 이 snapshot이다(Start와 Watch 사이 Sync의 변경을 잃지
        // 않는다). 일시적으로 적용하지 못한 값은 본 것으로 두지 않는다(다음 sync 지점에 다시
        // 시도한다).
        static_cast<void>(levelWatcher_.Changed());
        static_cast<void>(rateLimitsWatcher_.Changed());
        static_cast<void>(levelsWatcher_.Changed());
        if (!ApplyGlobalLevel()) {
            levelWatcher_.Reset();
        }

        if (!ApplyRateLimits()) {
            rateLimitsWatcher_.Reset();
        }

        if (!ApplyLevelOverrides()) {
            levelsWatcher_.Reset();
        }
    }

    return Result<void>::Success();
}

void LogHost::PrepareFileSink(StringView hostName) noexcept {
    if (!LogFileEnabledVar.Get()) {
        return;
    }

    FileLogSinkConfig file;
    file.directory = LogFileDirectoryVar.Get();
    file.hostName = hostName;
    file.format = LogFileFormatVar.Get();
    if (file.format == LogFileFormat::JsonLines) {
        if (!LogFileLayoutVar.Get().IsEmpty()) {
            Log::Warning(LogHostCategory,
                         "log.file.layout is ignored because log.file.format is jsonlines");
        }
    } else {
        file.layout = ResolveLayout(LogFileLayoutVar.Name(), LogFileLayoutVar.Get());
    }

    file.maxBytes = static_cast<uint64>(LogFileMaxBytesVar.Get());
    file.retention = static_cast<uint32>(LogFileRetentionVar.Get());
    constexpr uint64 NanosecondsPerMinute = 60ull * 1000 * 1000 * 1000;
    file.maxAgeNanoseconds = static_cast<uint64>(LogFileMaxAgeVar.Get()) * NanosecondsPerMinute;

    // 압축: 묶음마다 독립 zstd frame. 작업 공간은 여기서 한 번 잡는다(Open 뒤 무할당).
    if (LogFileCompressVar.Get()) {
        auto zstdSettings =
            CompressionSettings::TryMake(CompressionFormat::Zstd, LogFileCompressionLevel);
        auto fixed = zstdSettings ? FixedWorkspaceCompressor::TryCreate(zstdSettings.Value(),
                                                                        file.batchBytes, allocator_)
                                  : Result<FixedWorkspaceCompressor>::Failure(zstdSettings.Error());
        auto made =
            fixed ? UniquePtr<ZstdLogBatchCompressor>::TryMake(allocator_, Move(fixed.Value()))
                  : Result<UniquePtr<ZstdLogBatchCompressor>>::Failure(fixed.Error());
        if (!made) {
            Log::Error(LogHostCategory,
                       "Log file compression could not start: {Error}; writing uncompressed",
                       ErrorCodeName(made.Error()));
        } else {
            compressor_ = Move(made.Value());
            file.compressor = compressor_.Get();
        }
    }

    if (LogFileEncryptVar.Get()) {
        // 공개키가 없거나 올바르지 않으면 파일을 만들지 않는다(평문으로 내려가지 않는다).
        const StringView text = LogFileRecipientKeyVar.Get().TrimAscii();
        uint8 key[crypto::LogFilePublicKeyBytes + 3];
        auto decoded = text.IsEmpty() ? Result<usize>::Failure(ErrorCode::NotFound)
                                      : crypto::TryFromBase64(text, crypto::Base64Variant::Standard,
                                                              Span<uint8>(key));
        if (!decoded || decoded.Value() != crypto::LogFilePublicKeyBytes) {
            fileError_ = decoded ? ErrorCode::InvalidArgument : decoded.Error();
            Log::Error(
                LogHostCategory,
                "log.file.encrypt is on but log.file.recipient_key is {Problem}; no log file "
                "is written (never plaintext)",
                text.IsEmpty() ? "missing" : "not a base64 X-Wing public key");
            return;
        }

        // 압축한 묶음을 봉인하면 머리에 표시한다(읽는 쪽이 묶음마다 푼다).
        auto sealer = UniquePtr<crypto::LogFileSealer>::TryMake(
            allocator_, Span<const uint8>(key, crypto::LogFilePublicKeyBytes),
            file.compressor != nullptr ? crypto::LogFileFlagCompressedBatches : uint8{0});
        if (!sealer) {
            fileError_ = sealer.Error();
            Log::Error(LogHostCategory, "Log file sealer could not be created: {Error}",
                       ErrorCodeName(sealer.Error()));
            return;
        }

        sealer_ = Move(sealer.Value());
        file.cipher = sealer_.Get();
    }

    auto sink = UniquePtr<FileLogSink>::TryMake(allocator_, allocator_, file);
    if (!sink) {
        fileError_ = sink.Error();
        sealer_.Reset();
        return;
    }

    if (auto opened = sink.Value()->Open(); !opened) {
        fileError_ = opened.Error();
        sealer_.Reset();
        Log::Error(LogHostCategory,
                   "Log file sink could not open in {Directory}: {Error}; continuing without a log "
                   "file",
                   file.directory, ErrorCodeName(opened.Error()));
        return;
    }

    if (auto added = logs_->AddSink(*sink.Value()); !added) {
        fileError_ = added.Error();
        sink.Value()->Close();
        sealer_.Reset();
        return;
    }

    file_ = Move(sink.Value());
}

Result<void> LogHost::Watch(config::ConfigContext& settings) noexcept {
    if (!logs_ || watched_ != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto added = settings.OnSynced().TryAddLambda(
        [this](const config::SyncChanges&) noexcept { OnSynced(); });
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    synced_ = added.Value();
    watched_ = &settings;
    // Start가 읽은 snapshot 뒤에 바뀐 값(그 사이의 Sync)을 지금 적용한다.
    OnSynced();
    return Result<void>::Success();
}

void LogHost::Stop() noexcept {
    if (watched_ != nullptr) {
        static_cast<void>(watched_->OnSynced().Remove(synced_));
        synced_ = DelegateHandle{};
        watched_ = nullptr;
    }

    appliedLevelsBytes_ = 0;
    appliedRateLimitsBytes_ = 0;
    if (logs_) {
        logs_->Stop();
        if (logs_->Stats().writerAbandoned) {
            // 남은 출력 스레드가 sink에서 돌아오면 이 객체들을 쓴다. 파괴하면 소멸자의 Join이
            // 끝나지 않을 수 있으므로 프로세스 끝까지 놓아 둔다(헤더의 Stop 설명).
            writerAbandoned_ = true;
            (void)ConstructAt<AbandonedOutputs>(
                abandonedStorage_, AbandonedOutputs{Move(logs_), Move(stderr_), Move(file_),
                                                    Move(sealer_), Move(compressor_)});
            abandonedStorage_ = nullptr;
            return;
        }

        logs_.Reset();
    }

    if (file_) {
        file_->Close();
        file_.Reset();
    }

    sealer_.Reset();
    compressor_.Reset();
    stderr_.Reset();
    if (abandonedStorage_ != nullptr) {
        if (!allocator_.Deallocate(abandonedStorage_)) {
            platform::Fatal("LogHost could not return its reserved storage");
        }

        abandonedStorage_ = nullptr;
    }
}

LogSystem& LogHost::System() noexcept {
    if (!logs_) {
        platform::Fatal("LogHost::System called before a successful Start");
    }

    return *logs_;
}

StringView LogHost::FilePath() const noexcept {
    return file_ ? file_->CurrentPath() : StringView();
}

void LogHost::OnSynced() noexcept {
    if (watched_ == nullptr || !logs_) {
        return;
    }

    // 이 문맥의 최신 snapshot에서 바뀐 값만 다시 적용한다(같은 스레드의 다른 문맥 Sync와 섞이지
    // 않게 명시적으로 묶는다).
    const SharedPtr<const config::ConfigSnapshot> snapshot = watched_->Acquire();
    if (snapshot.Get() == nullptr) {
        return;
    }

    // 바뀐 값만 다시 적용한다. 일시적으로 실패한 값(LogSystem Busy — 출력 스레드·sink 안에서 불린
    // sync 지점)은 본 세대를 잊어 다음 sync 지점에 다시 시도한다.
    const bool level = levelWatcher_.Changed(*snapshot);
    const bool rateLimits = rateLimitsWatcher_.Changed(*snapshot);
    const bool levels = levelsWatcher_.Changed(*snapshot);
    cvar::SnapshotBinding bound(snapshot);
    if (level && !ApplyGlobalLevel()) {
        levelWatcher_.Reset();
    }

    if (rateLimits && !ApplyRateLimits()) {
        rateLimitsWatcher_.Reset();
    }

    if (levels && !ApplyLevelOverrides()) {
        levelsWatcher_.Reset();
    }
}

bool LogHost::ApplyGlobalLevel() noexcept {
    auto set = logs_->SetGlobalMinimum(LogLevelVar.Get());
    return set || set.Error() != ErrorCode::Busy;
}

bool LogHost::ApplyRateLimits() noexcept {
    // 내용이 그대로면 아무것도 하지 않는다(창이 초기화되지 않게). 바뀌면 빠진 접두사만 지우고, 남은
    // 접두사는 상한만 바꾼다(창·수 유지). 문법은 설정 단계에서 Validate(CheckLogRateLimitsSyntax)가
    // 거부하므로 여기서 남기는 Warning은 적용 실패(표 용량)다. 문법 실패 항목의 Warning은 방어다.
    const StringView list = LogRateLimitsVar.Get();
    const StringView applied(appliedRateLimits_, appliedRateLimitsBytes_);
    if (list == applied) {
        return true;
    }

    bool busy = false;
    ForEachEntry(applied, [&](StringView old) {
        StringView prefix;
        StringView count;
        if (!detail::SplitLogSettingEntry(old, prefix, count)) {
            return;
        }

        bool kept = false;
        ForEachEntry(list, [&](StringView entry) {
            StringView newPrefix;
            StringView newCount;
            kept = kept || (detail::SplitLogSettingEntry(entry, newPrefix, newCount) &&
                            EqualsIgnoringAsciiCase(newPrefix, prefix));
        });
        if (!kept) {
            auto cleared = logs_->ClearRateLimit(prefix);
            busy = busy || (!cleared && cleared.Error() == ErrorCode::Busy);
        }
    });
    appliedRateLimitsBytes_ = 0;
    if (list.ByteCount() > MaxLogRateLimitsBytes) {
        (void)logs_->ClearRateLimits();
        Log::Warning(LogHostCategory,
                     "log.rate_limits is longer than {Limit} bytes; rate limits are not applied",
                     MaxLogRateLimitsBytes);
        return true;
    }

    platform::MemoryCopy(appliedRateLimits_, list.Data(), list.ByteCount());
    appliedRateLimitsBytes_ = list.ByteCount();

    ForEachEntry(list, [&](StringView entry) {
        StringView prefix;
        StringView count;
        uint32 perSecond = 0;
        if (!detail::SplitLogSettingEntry(entry, prefix, count) ||
            !detail::ParseLogSettingCount(count, MaxLogRateLimitPerSecond, perSecond)) {
            Log::Warning(LogHostCategory,
                         "log.rate_limits entry '{Entry}' is not <category prefix>=<records per "
                         "second>; ignored",
                         entry);
            return;
        }

        if (auto set = logs_->SetRateLimit(prefix, perSecond); !set) {
            if (set.Error() == ErrorCode::Busy) {
                busy = true;
                return;
            }

            Log::Warning(LogHostCategory,
                         "log.rate_limits entry '{Entry}' was not applied: {Error}", entry,
                         ErrorCodeName(set.Error()));
        }
    });
    if (busy) {
        // 다음 적용이 모든 항목을 다시 넣게 한다(적용한 목록으로 남기지 않는다).
        appliedRateLimitsBytes_ = 0;
    }

    return !busy;
}

bool LogHost::ApplyLevelOverrides() noexcept {
    // 앞서 적용한 재정의를 지우고 새 목록을 적용한다. 문법은 설정 단계에서
    // Validate(CheckLogLevelsSyntax)가 거부하므로 여기서 남기는 Warning은 적용 실패(재정의 표
    // 용량·해시 충돌)다. 카테고리 목록은 닫혀 있지 않으므로(상수 객체 — 등록 표가 없다) 모르는
    // 접두사는 판정하지 않는다. 반환: 일시적 실패(Busy) 없이 끝났는지다.
    bool busy = false;
    ForEachEntry(StringView(appliedLevels_, appliedLevelsBytes_), [&](StringView entry) {
        StringView prefix;
        LogLevel ignored = LogLevel::Info;
        if (SplitEntry(entry, prefix, ignored)) {
            auto cleared = logs_->ClearLevel(prefix);
            busy = busy || (!cleared && cleared.Error() == ErrorCode::Busy);
        }
    });
    if (busy) {
        // 앞 재정의를 지우지 못했다(적용한 목록을 그대로 두고 다음 sync 지점에 다시 시도한다).
        return false;
    }

    appliedLevelsBytes_ = 0;
    const StringView list = LogLevelsVar.Get();
    if (list.ByteCount() > sizeof(appliedLevels_)) {
        Log::Warning(LogHostCategory,
                     "log.levels is longer than {Limit} bytes; per-category levels are not applied",
                     MaxLogLevelsBytes);
        return true;
    }

    platform::MemoryCopy(appliedLevels_, list.Data(), list.ByteCount());
    appliedLevelsBytes_ = list.ByteCount();
    ForEachEntry(list, [&](StringView entry) {
        StringView prefix;
        LogLevel entryLevel = LogLevel::Info;
        if (!SplitEntry(entry, prefix, entryLevel)) {
            Log::Warning(LogHostCategory,
                         "log.levels entry '{Entry}' is not <category prefix>=<level>; ignored",
                         entry);
            return;
        }

        if (auto set = logs_->SetLevel(prefix, entryLevel); !set) {
            if (set.Error() == ErrorCode::Busy) {
                busy = true;
                return;
            }

            Log::Warning(LogHostCategory, "log.levels entry '{Entry}' was not applied: {Error}",
                         entry, ErrorCodeName(set.Error()));
        }
    });
    return !busy;
}

} // namespace rocks
