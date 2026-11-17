#pragma once

#include <Config/ConfigContext.h>
#include <Core/Diagnostics/FileLogSink.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/UniquePtr.h>
#include <Crypto/LogFileSealer.h>

// 호스트의 로그 준비다(Docs/Design/LoggingSystem.ko.md 5.1·9.1절, LG9). 설정 시스템의 log.* 콘솔
// 설정을 등록하고, 그 값으로 LogSystem과 sink(stderr, 선택 파일, 선택 봉인 파일)를 만든다. Core는
// Config·Crypto 아래 계층이라 이 모듈이 그 위에서 묶는다. 엔진은 LogSystem을 만들거나 설치하지
// 않는다(호스트가 이 모듈로 만들고 LogInstallScope로 설치한다).
//
// 설정(이름 · 종류 · 기본값 · 플래그):
//   log.mode               string  "default"  시작 뒤 고정  default(호스트 프리셋)·async·sync
//   log.level              string  "info"     실행 중 변경  trace·debug·info·warning·error·fatal
//   log.levels             string  ""         실행 중 변경  "Graphics=warning,Engine.World=debug"
//   log.file.enabled       bool    false      시작 뒤 고정
//   log.file.directory     string  "Saved/Logs"  시작 뒤 고정
//   log.file.format        string  "text"     시작 뒤 고정  text·jsonlines
//   log.file.max_bytes     int64   16777216   시작 뒤 고정  65536–1099511627776
//   log.file.retention     int32   8          시작 뒤 고정  1–256
//   log.file.encrypt       bool    false      시작 뒤 고정
//   log.file.recipient_key string  ""         시작 뒤 고정  X-Wing 공개키 base64(비밀 아님)
// 레벨 이름과 log.levels의 접두사는 대소문자를 가리지 않는다. 실행 중 변경은 sync 지점
// (ConfigContext::Sync)에서 Watch가 적용한다.
//
// 사용 예(엔진 호스트):
//   REQUIRE(RegisterLogSettings(context));
//   ... 설정 파일 층을 넣고 context.Sync();
//   LogHost host(allocator);
//   if (!host.Start(LogSystemConfig::ForEngineHost(), &context, "Sandbox")) { ... }
//   LogInstallScope installed(host.System());
//   (void)host.Watch(context);
//   context.CompleteStartup();          // 그 뒤 log.mode·log.file.* 변경은 거부된다
//   ... 엔진 실행 ...
//   host.Stop();                        // 설치 범위가 끝난 뒤
namespace rocks {

inline constexpr StringView LogModeSetting = "log.mode";
inline constexpr StringView LogLevelSetting = "log.level";
inline constexpr StringView LogLevelsSetting = "log.levels";
inline constexpr StringView LogFileEnabledSetting = "log.file.enabled";
inline constexpr StringView LogFileDirectorySetting = "log.file.directory";
inline constexpr StringView LogFileFormatSetting = "log.file.format";
inline constexpr StringView LogFileMaxBytesSetting = "log.file.max_bytes";
inline constexpr StringView LogFileRetentionSetting = "log.file.retention";
inline constexpr StringView LogFileEncryptSetting = "log.file.encrypt";
inline constexpr StringView LogFileRecipientKeySetting = "log.file.recipient_key";

// log.levels 문자열의 최대 byte 수다.
inline constexpr usize MaxLogLevelsBytes = 1024;

// log.* 콘솔 설정을 등록한다(모듈 등록 함수에서 명시적으로 — 전역 생성자 없음). 이미 들어온 층
// 입력은 등록 때 적용된다. 오류: ConfigContext::RegisterConsoleSetting의 오류(이미 등록했으면
// AlreadyExists).
Result<void> RegisterLogSettings(config::ConfigContext& context) noexcept;

// 레벨 이름(trace·debug·info·warning·error·fatal, 대소문자 무시)을 읽는다. 모르는 이름이면 false다.
bool TryParseLogLevel(StringView text, LogLevel& level) noexcept;

// 설정으로 만든 LogSystem과 sink 묶음이다. 소유 스레드 하나에서 Start·Watch·Stop을 부른다.
class LogHost {
public:
    explicit LogHost(Allocator& allocator) noexcept;
    ~LogHost() noexcept;
    LogHost(const LogHost&) = delete;
    LogHost& operator=(const LogHost&) = delete;

    // preset(호스트 프리셋 — ForEngineHost·ForTool)에 settings의 log.* 값을 덮어써 LogSystem을
    // 만들고 시작한다. settings가 null이면 프리셋 그대로 stderr sink만 단다. hostName은 파일 이름
    // 앞부분이다(FileLogSinkConfig::hostName).
    // - 파일 기록을 켰는데 열지 못하면(폴더·권한) 파일 sink 없이 시작하고 Error를 한 번 남긴다.
    // - 암호화를 켰는데 공개키가 없거나 올바르지 않으면 파일 sink를 만들지 않는다(평문으로 내려가지
    //   않는다). Error를 한 번 남긴다. FileSetupError가 그 원인이다.
    // 오류: 이미 시작함 AlreadyExists, LogSystem 생성·시작 실패는 그 오류.
    Result<void> Start(const LogSystemConfig& preset, const config::ConfigContext* settings,
                       StringView hostName) noexcept;

    // log.level·log.levels 변경을 구독한다. 값이 바뀐 sync 지점에서 전역 최소와 카테고리 재정의를
    // 다시 적용한다. settings는 Stop까지 살아 있어야 한다. 오류: 시작 전 InvalidArgument, 구독
    // 오류.
    Result<void> Watch(config::ConfigContext& settings) noexcept;

    // 구독을 끝내고 LogSystem을 멈추고 파일을 닫는다. 설치 범위가 끝난 뒤 부른다. 두 번 불러도
    // 된다.
    void Stop() noexcept;

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
    static void OnLevelsChanged(void* context, const config::ConfigChange& change) noexcept;
    void ApplyLevels(const config::ConfigContext& settings) noexcept;
    void PrepareFileSink(const config::ConfigContext& settings, StringView hostName) noexcept;

    Allocator& allocator_;
    UniquePtr<LogSystem> logs_;
    StderrLogSink stderr_;
    UniquePtr<crypto::LogFileSealer> sealer_;
    UniquePtr<FileLogSink> file_;
    config::ConfigContext* watched_ = nullptr;
    config::ConfigSubscriptionId subscriptions_[2]{};
    uint32 subscriptionCount_ = 0;
    // 마지막으로 적용한 log.levels(지울 접두사를 알려고 복사해 둔다)다.
    char appliedLevels_[MaxLogLevelsBytes]{};
    usize appliedLevelsBytes_ = 0;
    ErrorCode fileError_ = ErrorCode::None;
};

} // namespace rocks
