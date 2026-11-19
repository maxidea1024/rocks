#pragma once

#include <Config/CVar.h>
#include <Config/ConfigContext.h>
#include <Core/Diagnostics/FileLogSink.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/UniquePtr.h>
#include <Crypto/LogFileSealer.h>

// 호스트의 로그 준비다(Docs/Design/LoggingSystem.ko.md 5.1·9.1·9.2절, LG9). 설정 시스템의 log.*
// CVar 핸들을 등록하고(RegisterLogSettings — CVarSystem.ko.md CVR7), 그 값으로 LogSystem과
// sink(stderr, 선택 파일, 선택 봉인 파일)를 만든다. Core는 Config·Crypto 아래 계층이라 이 모듈이 그
// 위에서 묶는다. 엔진은 LogSystem을 만들거나 설치하지 않는다(호스트가 이 모듈로 만들고
// LogInstallScope로 설치한다).
//
// 설정(아래 핸들 — 이름 · 타입 · 기본값):
//   log.mode               BootCVar<LogHostMode>    default   default(호스트 프리셋)·async·sync
//   log.level              CVar<LogLevel>           info      trace·debug·info·warning·error·fatal
//   log.levels             CVar<String>             ""        "Graphics=warning,Engine.World=debug"
//   log.rate_limits        CVar<String>             ""        "Graphics=20,Engine.World.Tick=100"
//   log.console.layout     BootCVar<String>         ""        레이아웃 패턴(빈 값은 기본 콘솔
//   레이아웃) log.file.enabled       BootCVar<bool>           false log.file.directory
//   BootCVar<String>         "Saved/Logs" log.file.format        BootCVar<LogFileFormat>  text
//   text·jsonlines log.file.layout        BootCVar<String>         ""        레이아웃
//   패턴(jsonlines면 쓰이지 않는다) log.file.max_bytes     BootCVar<int64>          16777216
//   65536–1099511627776 log.file.retention     BootCVar<int32>          8         1–256
//   log.file.max_age_minutes BootCVar<int32>        0         0–10080(0은 끔)
//   log.file.compress      BootCVar<bool>           false
//   log.file.encrypt       BootCVar<bool>           false
//   log.file.recipient_key BootCVar<String>         ""        X-Wing 공개키 base64(비밀 아님)
// enum 값은 대소문자를 가리지 않고 받는다(CVar 4.4절 — log.level의 "Warning"도 받는다). 문법이 틀린
// log.levels·log.rate_limits·레이아웃은 설정 단계에서 Validate가 거부한다(L18). 실행 중
// 변경(CVar)은 sync 지점(ConfigContext::Sync)에서 Watch가 적용하고, BootCVar는
// 봉인(CompleteStartup) 뒤 이번 실행 동안 고정이다. log.level은 늘 값이 있으므로(기본 info)
// 프리셋의 globalMinimum을 덮어쓴다. 설정 문맥 없이 Start하면 프리셋 그대로다.
//
// 사용 예(엔진 호스트):
//   REQUIRE(RegisterLogSettings(context));
//   ... 설정 파일 층을 넣고 context.Sync(); context.CompleteStartup();
//   LogHost host(allocator);
//   if (!host.Start(LogSystemConfig::ForEngineHost(), &context, "Sandbox")) { ... }
//   LogInstallScope installed(host.System());
//   (void)host.Watch(context);
//   ... 엔진 실행 ...
//   host.Stop();                        // 설치 범위가 끝난 뒤
//   if (host.WriterAbandoned()) { 정리 없이 프로세스 종료(_Exit) }
namespace rocks {

// log.mode 값이다(호스트 프리셋 그대로, 또는 LogMode::Async·Sync로 덮어쓰기).
enum class LogHostMode : uint8 {
    Default,
    Async,
    Sync
};

// log.mode의 이름 표다(CVar enum — Config/CVarEnum.h).
constexpr Span<const StringView> CVarEnumNames(LogHostMode) noexcept {
    static constexpr StringView names[] = {"default", "async", "sync"};
    return names;
}

// log.level의 이름 표다(LogLevel 값 순서 — trace·debug·info·warning·error·fatal).
constexpr Span<const StringView> CVarEnumNames(LogLevel) noexcept {
    static constexpr StringView names[] = {"trace", "debug", "info", "warning", "error", "fatal"};
    return names;
}

// log.file.format의 이름 표다.
constexpr Span<const StringView> CVarEnumNames(LogFileFormat) noexcept {
    static constexpr StringView names[] = {"text", "jsonlines"};
    return names;
}

// log.file.max_age_minutes의 상한(7일)이다.
inline constexpr int32 MaxLogFileAgeMinutes = 10080;

// log.file.compress가 쓰는 zstd 수준이다.
inline constexpr int32 LogFileCompressionLevel = 3;

// log.levels 문자열의 최대 byte 수다.
inline constexpr usize MaxLogLevelsBytes = 1024;

// log.rate_limits 문자열의 최대 byte 수다.
inline constexpr usize MaxLogRateLimitsBytes = 1024;

// log.* 핸들을 등록한다(모듈 등록 함수에서 명시적으로 — 전역 생성자 없음). 이미 들어온 층 입력은
// 등록 때 적용된다. 오류: ConfigContext::Register의 오류(같은 이름을 다른 핸들이 쓰면
// AlreadyExists·TypeMismatch). 같은 문맥에 다시 부르면 성공이다(멱등).
Result<void> RegisterLogSettings(config::ConfigContext& context) noexcept;

// 레벨 이름(trace·debug·info·warning·error·fatal, 대소문자 무시)을 읽는다. 모르는 이름이면 false다.
bool TryParseLogLevel(StringView text, LogLevel& level) noexcept;

namespace detail {

// 설정에 쓰는 레벨 이름이다(대소문자 무시). 순서는 LogLevel 값과 같다.
inline constexpr StringView LogLevelSettingNames[] = {"trace",   "debug", "info",
                                                      "warning", "error", "fatal"};

constexpr bool IsLogSettingSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// 앞뒤 ASCII 공백을 뺀 view다(상수 평가용).
constexpr StringView TrimLogSettingText(StringView text) noexcept {
    usize start = 0;
    usize end = text.ByteCount();
    while (start < end && IsLogSettingSpace(text.Data()[start])) {
        ++start;
    }

    while (end > start && IsLogSettingSpace(text.Data()[end - 1])) {
        --end;
    }

    return StringView(text.Data() + start, end - start);
}

constexpr bool IsLogLevelSettingName(StringView text) noexcept {
    for (StringView name : LogLevelSettingNames) {
        if (EqualsIgnoringAsciiCase(text, name)) {
            return true;
        }
    }

    return false;
}

// "접두사=값" 목록의 항목 하나를 나눈다. '='이 없으면 false다.
constexpr bool SplitLogSettingEntry(StringView entry, StringView& prefix,
                                    StringView& value) noexcept {
    for (usize index = 0; index < entry.ByteCount(); ++index) {
        if (entry.Data()[index] == '=') {
            prefix = TrimLogSettingText(StringView(entry.Data(), index));
            value = TrimLogSettingText(
                StringView(entry.Data() + index + 1, entry.ByteCount() - index - 1));
            return true;
        }
    }

    return false;
}

} // namespace detail

// log.levels 문자열("Graphics=warning,Engine.World=debug")의 문법만 검사한다(설정 검증용, CVar
// Validate). 통과하면 빈 view, 아니면 영어 리터럴 이유 문장이다. 순수 함수이고 상수 평가할 수 있다.
// 빈 항목은 건너뛴다. 항목별 위치 진단과 적용 실패(표 용량·해시 충돌)는 LogHost가 적용 때 남긴다.
//
//   static_assert(CheckLogLevelsSyntax("Graphics=Warning, Engine.World=debug").IsEmpty());
constexpr StringView CheckLogLevelsSyntax(StringView text) noexcept {
    if (text.ByteCount() > MaxLogLevelsBytes) {
        return "log.levels is longer than MaxLogLevelsBytes";
    }

    usize start = 0;
    while (start <= text.ByteCount()) {
        usize end = start;
        while (end < text.ByteCount() && text.Data()[end] != ',') {
            ++end;
        }

        const StringView entry =
            detail::TrimLogSettingText(StringView(text.Data() + start, end - start));
        start = end + 1;
        if (entry.IsEmpty()) {
            continue;
        }

        StringView prefix;
        StringView level;
        if (!detail::SplitLogSettingEntry(entry, prefix, level)) {
            return "a log.levels entry is not <category prefix>=<level>";
        }

        if (ValidateLogCategoryName(prefix) != LogCategoryNameError::None) {
            return "a log.levels category prefix is not a valid category name";
        }

        if (!detail::IsLogLevelSettingName(level)) {
            return "a log.levels level is not trace, debug, info, warning, error or fatal";
        }
    }

    return StringView();
}

namespace detail {

// 10진 양의 정수를 읽는다(상수 평가용). 숫자가 아니거나 maximum을 넘으면 false다.
constexpr bool ParseLogSettingCount(StringView text, uint32 maximum, uint32& value) noexcept {
    if (text.IsEmpty()) {
        return false;
    }

    uint64 total = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char digit = text.Data()[index];
        if (digit < '0' || digit > '9') {
            return false;
        }

        total = total * 10 + static_cast<uint64>(digit - '0');
        if (total > maximum) {
            return false;
        }
    }

    value = static_cast<uint32>(total);
    return true;
}

} // namespace detail

// log.rate_limits 문자열("Graphics=20,Engine.World.Tick=100", 접두사별 초당 기록 수)의 문법만
// 검사한다(설정 검증용, CVar Validate). 통과하면 빈 view, 아니면 영어 리터럴 이유 문장이다. 순수
// 함수이고 상수 평가할 수 있다. 항목은 MaxLogRateLimits개까지, 수는 MinLogRateLimitPerSecond–
// MaxLogRateLimitPerSecond다.
//
//   static_assert(CheckLogRateLimitsSyntax("Graphics=20, Engine.World.Tick=100").IsEmpty());
constexpr StringView CheckLogRateLimitsSyntax(StringView text) noexcept {
    if (text.ByteCount() > MaxLogRateLimitsBytes) {
        return "log.rate_limits is longer than MaxLogRateLimitsBytes";
    }

    usize entries = 0;
    usize start = 0;
    while (start <= text.ByteCount()) {
        usize end = start;
        while (end < text.ByteCount() && text.Data()[end] != ',') {
            ++end;
        }

        const StringView entry =
            detail::TrimLogSettingText(StringView(text.Data() + start, end - start));
        start = end + 1;
        if (entry.IsEmpty()) {
            continue;
        }

        StringView prefix;
        StringView count;
        if (!detail::SplitLogSettingEntry(entry, prefix, count)) {
            return "a log.rate_limits entry is not <category prefix>=<records per second>";
        }

        if (ValidateLogCategoryName(prefix) != LogCategoryNameError::None) {
            return "a log.rate_limits category prefix is not a valid category name";
        }

        uint32 value = 0;
        if (!detail::ParseLogSettingCount(count, MaxLogRateLimitPerSecond, value) ||
            value < MinLogRateLimitPerSecond) {
            return "a log.rate_limits value is not a whole number from 1 to 1000000";
        }

        if (++entries > MaxLogRateLimits) {
            return "log.rate_limits has more than MaxLogRateLimits entries";
        }
    }

    return StringView();
}

// log.* 핸들이다(정의는 LogHost.cpp, 등록은 RegisterLogSettings — 위 표).
extern BootCVar<LogHostMode> LogModeVar;
extern CVar<LogLevel> LogLevelVar;
extern CVar<String> LogLevelsVar;
extern CVar<String> LogRateLimitsVar;
extern BootCVar<String> LogConsoleLayoutVar;
extern BootCVar<bool> LogFileEnabledVar;
extern BootCVar<String> LogFileDirectoryVar;
extern BootCVar<LogFileFormat> LogFileFormatVar;
extern BootCVar<String> LogFileLayoutVar;
extern BootCVar<int64> LogFileMaxBytesVar;
extern BootCVar<int32> LogFileRetentionVar;
extern BootCVar<int32> LogFileMaxAgeVar;
extern BootCVar<bool> LogFileCompressVar;
extern BootCVar<bool> LogFileEncryptVar;
extern BootCVar<String> LogFileRecipientKeyVar;

// 설정으로 만든 LogSystem과 sink 묶음이다. 소유 스레드 하나에서 Start·Watch·Stop을 부른다.
class LogHost {
public:
    explicit LogHost(Allocator& allocator) noexcept;
    ~LogHost() noexcept;
    LogHost(const LogHost&) = delete;
    LogHost& operator=(const LogHost&) = delete;

    // preset(호스트 프리셋 — ForEngineHost·ForTool)에 settings의 log.* 값을 덮어써 LogSystem을
    // 만들고 시작한다. settings가 null이면 프리셋 그대로 stderr sink만 단다. settings는 봉인
    // (CompleteStartup) 뒤여야 한다(부팅 값을 읽는다 — settings의 최신 snapshot을 묶어 읽는다).
    // hostName은 파일 이름 앞부분이다(FileLogSinkConfig::hostName).
    // - 파일 기록을 켰는데 열지 못하면(폴더·권한) 파일 sink 없이 시작하고 Error를 한 번 남긴다.
    // - 암호화를 켰는데 공개키가 없거나 올바르지 않으면 파일 sink를 만들지 않는다(평문으로 내려가지
    //   않는다). Error를 한 번 남긴다. FileSetupError가 그 원인이다.
    // 오류: 이미 시작함 AlreadyExists, 앞 Stop이 출력 스레드를 남겨 둠(WriterAbandoned) Busy,
    // settings가 초기화 전·봉인 전 InvalidArgument, LogSystem 생성·시작 실패는 그 오류.
    Result<void> Start(const LogSystemConfig& preset, const config::ConfigContext* settings,
                       StringView hostName) noexcept;

    // log.level·log.levels·log.rate_limits의 실행 중 변경을 따른다: settings의 OnSynced에 알림
    // 하나를 더하고, Sync마다 그 snapshot에서 값이 바뀐 것(Watcher — 세대 비교)만 다시
    // 적용한다(전역 최소·카테고리 재정의·속도 제한). settings는 Stop까지 살아 있어야 한다. 소유
    // 스레드(게임 lane)에서 부른다. 오류: 시작 전·이미 따르는 중 InvalidArgument, 알림 추가 오류.
    Result<void> Watch(config::ConfigContext& settings) noexcept;

    // 변경 알림을 빼고 LogSystem을 멈추고 파일을 닫는다. 설치 범위가 끝난 뒤 부른다. 두 번 불러도
    // 된다.
    //
    // 멈춘 sink 때문에 LogSystem::Stop이 출력 스레드를 남겨 두면(Stats().writerAbandoned)
    // LogSystem을 파괴하지 않는다(소멸자의 Join이 끝나지 않을 수 있다). 대신 LogSystem·stderr
    // sink·파일 sink· 봉인기를 일부러 놓아 프로세스 끝까지 살려 둔다(남은 스레드가 돌아오면 그
    // sink들을 쥐고 있다). 생성자에 준 allocator도 프로세스 끝까지 살아 있어야 한다. 이때
    // WriterAbandoned가 참이고, 호스트는 정리(전역 소멸자·CRT 종료 처리) 없이 프로세스를 끝낸다(예:
    // _Exit). 그 뒤 Start는 Busy다.
    void Stop() noexcept;

    // 앞 Stop이 멈춘 sink에 걸린 출력 스레드를 남겨 두고 LogSystem과 sink를 놓았는지다(Stop 설명).
    bool WriterAbandoned() const noexcept {
        return writerAbandoned_;
    }

    // 시작한 LogSystem이다. 전제: Start 성공 — 아니면 Fatal.
    LogSystem& System() noexcept;

    bool IsFileLogging() const noexcept {
        return file_.Get() != nullptr;
    }

    // 지금 쓰는 로그 파일 경로다(파일 기록이 없으면 빈 값).
    StringView FilePath() const noexcept;

    // 파일 sink를 준비하지 못한 원인이다(없거나 파일 기록을 끄면 None).
    ErrorCode FileSetupError() const noexcept {
        return fileError_;
    }

private:
    // Start의 본체다. settings면 설정 문맥의 snapshot이 이 스레드에 묶여 있다(핸들로 읽는다).
    Result<void> StartWith(const LogSystemConfig& preset, bool settings,
                           StringView hostName) noexcept;
    // 아래 셋과 PrepareFileSink는 settings의 snapshot이 이 스레드에 묶여 있을 때 부른다(핸들로
    // 읽는다).
    void ApplyGlobalLevel() noexcept;
    void ApplyLevelOverrides() noexcept;
    void ApplyRateLimits() noexcept;
    void PrepareFileSink(StringView hostName) noexcept;
    void OnSynced() noexcept;

    // Stop이 출력 스레드를 남길 때 LogSystem과 sink를 옮겨 놓고 파괴하지 않는 묶음이다.
    struct AbandonedOutputs;

    Allocator& allocator_;
    UniquePtr<LogSystem> logs_;
    // 출력 스레드를 남겨 둔 Stop이 LogSystem과 함께 놓을 수 있게 힙에 둔다.
    UniquePtr<StderrLogSink> stderr_;
    UniquePtr<crypto::LogFileSealer> sealer_;
    // log.file.compress의 묶음 압축기(고정 작업 공간 zstd)다.
    UniquePtr<LogBatchCompressor> compressor_;
    UniquePtr<FileLogSink> file_;
    config::ConfigContext* watched_ = nullptr;
    // watched_의 OnSynced에 더한 알림이다.
    DelegateHandle synced_{};
    // 실행 중 값 셋의 마지막으로 적용한 세대다(Watch가 지금 세대로 맞춘다).
    cvar::Watcher<LogLevel> levelWatcher_{LogLevelVar};
    cvar::Watcher<String> levelsWatcher_{LogLevelsVar};
    cvar::Watcher<String> rateLimitsWatcher_{LogRateLimitsVar};
    // 마지막으로 적용한 log.levels(지울 접두사를 알려고 복사해 둔다)다.
    char appliedLevels_[MaxLogLevelsBytes]{};
    usize appliedLevelsBytes_ = 0;
    // 마지막으로 적용한 log.rate_limits(바뀐 칸만 고치려고 복사해 둔다)다.
    char appliedRateLimits_[MaxLogRateLimitsBytes]{};
    usize appliedRateLimitsBytes_ = 0;
    ErrorCode fileError_ = ErrorCode::None;
    // Start가 미리 잡아 둔 AbandonedOutputs 저장소다(Stop 때 할당 실패가 없게). 정상 Stop은
    // 돌려준다.
    void* abandonedStorage_ = nullptr;
    bool writerAbandoned_ = false;
};

} // namespace rocks
