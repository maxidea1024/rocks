#include <LogHost/LogHost.h>

#include <Core/Diagnostics/Log.h>
#include <Crypto/Bytes.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

// 이 모듈이 남기는 진단의 카테고리다("Log" 아래).
constexpr LogCategory LogHostCategory{"Log.Host"};

constexpr StringView ModeValues[] = {"default", "async", "sync"};
constexpr StringView LevelValues[] = {"trace", "debug", "info", "warning", "error", "fatal"};
constexpr StringView FormatValues[] = {"text", "jsonlines"};

// 파일 크기 상한의 범위다(64 KiB – 1 TiB).
constexpr double MinLogFileMaxBytes = 64.0 * 1024;
constexpr double MaxLogFileMaxBytes = 1024.0 * 1024 * 1024 * 1024;

uint32 Flags(config::ConsoleSettingFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

const FieldValue* ValueOf(const config::ConfigContext& settings, StringView name) noexcept {
    auto id = settings.FindConsoleSetting(name);
    return id ? &settings.ConsoleValue(id.Value()) : nullptr;
}

StringView TextOf(const config::ConfigContext& settings, StringView name) noexcept {
    const FieldValue* value = ValueOf(settings, name);
    return value != nullptr ? value->string.View() : StringView();
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

        const StringView entry = StringView(list.Data() + start, end - start).TrimAscii();
        if (!entry.IsEmpty()) {
            visit(entry);
        }

        start = end + 1;
    }
}

// 항목 하나를 접두사와 레벨로 나눈다.
bool SplitEntry(StringView entry, StringView& prefix, LogLevel& level) noexcept {
    for (usize index = 0; index < entry.ByteCount(); ++index) {
        if (entry.Data()[index] == '=') {
            prefix = StringView(entry.Data(), index).TrimAscii();
            const StringView name =
                StringView(entry.Data() + index + 1, entry.ByteCount() - index - 1).TrimAscii();
            return !prefix.IsEmpty() && TryParseLogLevel(name, level);
        }
    }

    return false;
}

config::ConsoleSettingDefinition Define(StringView name, StringView help,
                                        config::ConsoleValueType type, StringView defaultValue,
                                        uint32 flags) noexcept {
    config::ConsoleSettingDefinition definition;
    definition.name = name;
    definition.help = help;
    definition.type = type;
    definition.defaultValue = defaultValue;
    definition.flags = flags;
    return definition;
}

} // namespace

bool TryParseLogLevel(StringView text, LogLevel& level) noexcept {
    for (usize index = 0; index < LogLevelCount; ++index) {
        if (EqualsIgnoringAsciiCase(text, LevelValues[index])) {
            level = static_cast<LogLevel>(index);
            return true;
        }
    }

    return false;
}

Result<void> RegisterLogSettings(config::ConfigContext& context) noexcept {
    using config::ConsoleSettingFlags;
    using config::ConsoleValueType;
    const uint32 startup = Flags(ConsoleSettingFlags::StartupOnly);

    config::ConsoleSettingDefinition mode =
        Define(LogModeSetting,
               "Log recording mode: default (the host preset), async (writer thread; engine hosts) "
               "or sync "
               "(the calling thread renders and writes; tools). Sync on an engine host makes the "
               "main tick "
               "wait for every sink. Fixed after startup.",
               ConsoleValueType::String, "default", startup);
    mode.allowedValues = ModeValues;
    config::ConsoleSettingDefinition level = Define(
        LogLevelSetting,
        "Global minimum log level: trace, debug, info, warning, error or fatal. Levels below "
        "the compiled minimum stay compiled out. Applied at the next config sync.",
        ConsoleValueType::String, "info", 0);
    level.allowedValues = LevelValues;
    const config::ConsoleSettingDefinition levels =
        Define(LogLevelsSetting,
               "Per-category log levels as comma-separated prefix=level pairs, for example "
               "\"Graphics=warning,Engine.World=debug\". Prefixes match dotted category names "
               "case-insensitively (longest prefix wins). Applied at the next config sync.",
               ConsoleValueType::String, "", 0);
    const config::ConsoleSettingDefinition enabled =
        Define(LogFileEnabledSetting, "Write log records to files. Fixed after startup.",
               ConsoleValueType::Bool, "false", startup);
    const config::ConsoleSettingDefinition directory =
        Define(LogFileDirectorySetting,
               "Folder for log files (<host>-<UTC start>.log). Fixed after startup.",
               ConsoleValueType::String, "Saved/Logs", startup);
    config::ConsoleSettingDefinition format =
        Define(LogFileFormatSetting,
               "Log file contents: text (the console layout) or jsonlines. Fixed after startup.",
               ConsoleValueType::String, "text", startup);
    format.allowedValues = FormatValues;
    config::ConsoleSettingDefinition maxBytes =
        Define(LogFileMaxBytesSetting,
               "Size limit of one log file in bytes; a new file starts when the next batch would "
               "pass it. Fixed after startup.",
               ConsoleValueType::Int64, "16777216", startup);
    maxBytes.hasRange = true;
    maxBytes.minimum = MinLogFileMaxBytes;
    maxBytes.maximum = MaxLogFileMaxBytes;
    config::ConsoleSettingDefinition retention =
        Define(LogFileRetentionSetting,
               "Number of this host's log files kept in the folder, including the current one; "
               "the oldest are deleted. Fixed after startup.",
               ConsoleValueType::Int32, "8", startup);
    retention.hasRange = true;
    retention.minimum = 1;
    retention.maximum = MaxLogFileRetention;
    const config::ConsoleSettingDefinition encrypt = Define(
        LogFileEncryptSetting,
        "Seal log files to the developer public key (log.file.recipient_key). If the key is "
        "missing or invalid no log file is written; there is never a plaintext fallback. Fixed "
        "after startup.",
        ConsoleValueType::Bool, "false", startup);
    const config::ConsoleSettingDefinition recipient = Define(
        LogFileRecipientKeySetting,
        "Developer X-Wing public key for log.file.encrypt, base64 (RocksCommandlet log keygen). "
        "It is not a secret. The private key never goes into the repository or cooked output. "
        "Fixed after startup.",
        ConsoleValueType::String, "", startup);
    const config::ConsoleSettingDefinition* definitions[] = {
        &mode,   &level,    &levels,    &enabled, &directory,
        &format, &maxBytes, &retention, &encrypt, &recipient};
    for (const config::ConsoleSettingDefinition* definition : definitions) {
        auto registered = context.RegisterConsoleSetting(*definition);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }
    }

    return Result<void>::Success();
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

    LogSystemConfig config = preset;
    if (settings != nullptr) {
        const StringView mode = TextOf(*settings, LogModeSetting);
        if (mode == "async") {
            config.mode = LogMode::Async;
        } else if (mode == "sync") {
            config.mode = LogMode::Sync;
        }

        LogLevel level = config.globalMinimum;
        if (TryParseLogLevel(TextOf(*settings, LogLevelSetting), level)) {
            config.globalMinimum = level;
        }
    }

    auto made = UniquePtr<LogSystem>::TryMake(allocator_, allocator_, config);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    logs_ = Move(made.Value());
    fileError_ = ErrorCode::None;
    if (auto added = logs_->AddSink(stderr_); !added) {
        logs_.Reset();
        return added;
    }

    if (settings != nullptr) {
        PrepareFileSink(*settings, hostName);
    }

    if (auto started = logs_->Start(); !started) {
        Stop();
        return started;
    }

    if (settings != nullptr) {
        ApplyLevels(*settings);
    }

    return Result<void>::Success();
}

void LogHost::PrepareFileSink(const config::ConfigContext& settings, StringView hostName) noexcept {
    const FieldValue* enabled = ValueOf(settings, LogFileEnabledSetting);
    if (enabled == nullptr || !enabled->boolean) {
        return;
    }

    FileLogSinkConfig file;
    file.directory = TextOf(settings, LogFileDirectorySetting);
    file.hostName = hostName;
    file.format = TextOf(settings, LogFileFormatSetting) == "jsonlines" ? LogFileFormat::JsonLines
                                                                        : LogFileFormat::Text;
    if (const FieldValue* maxBytes = ValueOf(settings, LogFileMaxBytesSetting)) {
        file.maxBytes = static_cast<uint64>(maxBytes->signedInteger);
    }

    if (const FieldValue* retention = ValueOf(settings, LogFileRetentionSetting)) {
        file.retention = static_cast<uint32>(retention->signedInteger);
    }

    const FieldValue* encrypt = ValueOf(settings, LogFileEncryptSetting);
    if (encrypt != nullptr && encrypt->boolean) {
        // 공개키가 없거나 올바르지 않으면 파일을 만들지 않는다(평문으로 내려가지 않는다).
        const StringView text = TextOf(settings, LogFileRecipientKeySetting).TrimAscii();
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

        auto sealer = UniquePtr<crypto::LogFileSealer>::TryMake(
            allocator_, Span<const uint8>(key, crypto::LogFilePublicKeyBytes));
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

    const StringView names[] = {LogLevelSetting, LogLevelsSetting};
    for (StringView name : names) {
        auto subscribed = settings.Subscribe(name, &LogHost::OnLevelsChanged, this);
        if (!subscribed) {
            for (uint32 index = 0; index < subscriptionCount_; ++index) {
                (void)settings.Unsubscribe(subscriptions_[index]);
            }

            subscriptionCount_ = 0;
            return Result<void>::Failure(subscribed.Error());
        }

        subscriptions_[subscriptionCount_++] = subscribed.Value();
    }

    watched_ = &settings;
    return Result<void>::Success();
}

void LogHost::Stop() noexcept {
    if (watched_ != nullptr) {
        for (uint32 index = 0; index < subscriptionCount_; ++index) {
            (void)watched_->Unsubscribe(subscriptions_[index]);
        }

        subscriptionCount_ = 0;
        watched_ = nullptr;
    }

    if (logs_) {
        logs_->Stop();
        logs_.Reset();
    }

    if (file_) {
        file_->Close();
        file_.Reset();
    }

    sealer_.Reset();
    appliedLevelsBytes_ = 0;
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

void LogHost::OnLevelsChanged(void* context, const config::ConfigChange&) noexcept {
    auto& host = *static_cast<LogHost*>(context);
    if (host.watched_ != nullptr && host.logs_) {
        host.ApplyLevels(*host.watched_);
    }
}

void LogHost::ApplyLevels(const config::ConfigContext& settings) noexcept {
    LogLevel level = LogLevel::Info;
    if (TryParseLogLevel(TextOf(settings, LogLevelSetting), level)) {
        (void)logs_->SetGlobalMinimum(level);
    }

    // 앞서 적용한 재정의를 지우고 새 목록을 적용한다.
    ForEachEntry(StringView(appliedLevels_, appliedLevelsBytes_), [&](StringView entry) {
        StringView prefix;
        LogLevel ignored = LogLevel::Info;
        if (SplitEntry(entry, prefix, ignored)) {
            (void)logs_->ClearLevel(prefix);
        }
    });
    appliedLevelsBytes_ = 0;
    const StringView list = TextOf(settings, LogLevelsSetting);
    if (list.ByteCount() > sizeof(appliedLevels_)) {
        Log::Warning(LogHostCategory,
                     "log.levels is longer than {Limit} bytes; per-category levels are not applied",
                     MaxLogLevelsBytes);
        return;
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
            Log::Warning(LogHostCategory, "log.levels entry '{Entry}' was not applied: {Error}",
                         entry, ErrorCodeName(set.Error()));
        }
    });
}

} // namespace rocks
