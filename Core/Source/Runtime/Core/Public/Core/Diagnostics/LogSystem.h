#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Containers/BoundedMpscQueue.h>
#include <Core/Diagnostics/LogBlockPool.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Diagnostics/LogCategory.h>
#include <Core/Diagnostics/LogEvent.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

// 로그 기록 경로의 소유자다(Docs/Design/LoggingSystem.ko.md 5·7·8·10절, UE FOutputDeviceRedirector
// 대응).
//
// 쓰는 순서:
//   LogSystem logs(allocator, LogSystemConfig::ForEngineHost());
//   logs.AddSink(stderrSink);
//   if (!logs.Start()) { ... }
//   LogInstallScope installed(logs);   // 이 범위 동안 Log::* 가 logs로 간다
//   Log::Info(LogEngine, "Engine initialized with {Count} subsystems", count);
//   // 범위가 끝나면 이전 설치 대상으로 돌아간다. 그 뒤 logs를 파괴해도 된다.
//
// 모드(LogMode):
// - Async: 호출 스레드는 캡처·큐 복사만 하고, 출력 스레드("LogWriter")가 렌더·sink 출력을 한다.
// - Sync: 호출 스레드가 렌더하고 sink까지 부른다(명령줄 도구). 큐·출력 스레드가 없다. 큰 값은
//   다른 모드와 같은 블록 풀을 써서 Manual과 같은 출력을 낸다(설계 5.1절).
// - Manual: Async와 같은 큐를 쓰지만 출력 스레드 없이 Pump()를 부른 스레드가 비운다(시험).
//
// 메모리: Start에서 큐·블록 풀·렌더 버퍼를 모두 확보한다. 그 뒤 생산자·출력 스레드 모두 힙 할당이
// 없다. 모든 함수는 noexcept이고, 로깅 경로의 실패는 버림·집계로 끝난다(Fatal·assert로 이어지지
// 않는다).
namespace rocks {

// 기록 한 건을 담는 큐 칸·준비 버퍼의 크기다(캡처 머리·값 칸·짧은 문자열).
inline constexpr usize LogSlotBytes = 512;

// 등록할 수 있는 sink 수다.
inline constexpr usize MaxLogSinks = 16;

// 카테고리 레벨 재정의 표의 칸 수다.
inline constexpr usize LogLevelOverrideCapacity = 64;

// 카테고리별 rate limit 표의 칸 수다.
inline constexpr usize MaxLogRateLimits = 16;

// rate limit 하나의 초당 상한 범위다.
inline constexpr uint32 MinLogRateLimitPerSecond = 1;
inline constexpr uint32 MaxLogRateLimitPerSecond = 1000000;

// 스레드 이름 표의 칸 수와 이름 하나의 최대 byte 수다.
inline constexpr usize LogThreadNameCapacity = 64;
inline constexpr usize MaxLogThreadNameBytes = 63;

// 같은 sink가 연속으로 이만큼 실패하면 막는다(비상 경로로 한 번 알린다).
inline constexpr uint32 LogSinkFailureLimit = 8;

// 막힌 sink를 이만큼의 묶음마다 한 번 다시 시도한다.
inline constexpr uint32 LogSinkRetryBatches = 256;

static_assert(LogCaptureMinimumBytes(MaxLogArguments) < LogSlotBytes,
              "a log slot must hold the capture header and every value cell");

enum class LogMode : uint8 {
    Async,
    Sync,
    Manual
};

// 큐가 가득 찼을 때의 생산자 동작이다.
// - Drop: 기다리지 않고 버린 뒤 레벨별로 센다(기본, 게임 스레드).
// - WaitForSpace: 공간이 생길 때까지 출력 스레드를 깨우며 양보한다(손실이 더 나쁜 도구 호스트,
//   Async 모드에서만 의미가 있다. Manual에서는 기다릴 출력 스레드가 없어 Drop처럼 동작한다).
enum class LogOverflow : uint8 {
    Drop,
    WaitForSpace
};

// LogSystem의 설정이다. Start 뒤에는 바꿀 수 없다.
struct LogSystemConfig {
    static constexpr usize DefaultQueueSlots = 4096;
    static constexpr usize DefaultBlockCount = 64;
    static constexpr usize DefaultBlockBytes = 4096;
    static constexpr usize DefaultRenderBytes = 64 * 1024;
    static constexpr uint32 DefaultDrainTimeoutMilliseconds = 5000;
    static constexpr uint32 DefaultFatalFlushTimeoutMilliseconds = 2000;
    // 큐 사용량이 이 백분율 이상이면 Info 이하를 버린다(남은 칸은 Warning 이상 전용).
    static constexpr uint32 SaturationPercent = 90;

    LogMode mode = LogMode::Async;
    LogOverflow overflow = LogOverflow::Drop;
    // 런타임 기본 최소 레벨(설계 7절 표: 모든 구성에서 Info).
    LogLevel globalMinimum = LogLevel::Info;
    // 큐 칸 수(2의 거듭제곱). Sync에서는 쓰지 않는다.
    usize queueSlots = DefaultQueueSlots;
    // 큰 값 블록의 수와 크기(모든 모드).
    usize blockCount = DefaultBlockCount;
    usize blockBytes = DefaultBlockBytes;
    // 출력 렌더 버퍼(메시지·속성 텍스트)의 크기다.
    usize renderBytes = DefaultRenderBytes;
    // Stop의 drain 대기 상한(best effort), Log::Fatal의 flush 대기 상한이다.
    uint32 drainTimeoutMilliseconds = DefaultDrainTimeoutMilliseconds;
    uint32 fatalFlushTimeoutMilliseconds = DefaultFatalFlushTimeoutMilliseconds;
    // 출력 스레드 스택 크기(0은 플랫폼 기본, platform::ThreadOptions::stackBytes).
    usize writerStackBytes = 0;
    // 기록 시각을 읽는 단조 tick 시계와 그 초당 tick 수다(시험 주입용). nullptr·0이면
    // platform::MonotonicTicks·MonotonicTickFrequency다. rate limit 창과 캡처 시각이 이 값을 쓴다.
    uint64 (*clock)() noexcept = nullptr;
    uint64 clockFrequency = 0;

    // tick이 있는 엔진 호스트용(Async)이다.
    static LogSystemConfig ForEngineHost() noexcept {
        return LogSystemConfig{};
    }

    // 명령줄 도구용(Sync)이다.
    static LogSystemConfig ForTool() noexcept {
        LogSystemConfig config;
        config.mode = LogMode::Sync;
        return config;
    }
};

// LogSystem의 누적 통계다(시험·진단 명령용). 값은 근사 스냅샷이다.
struct LogSystemStats {
    // 큐 포화(또는 Start 전·비상 모드의 낮은 레벨)로 버린 기록 수, 레벨별
    uint64 dropped[LogLevelCount]{};
    // sink까지 간 기록 수
    uint64 written = 0;
    // 값·메시지가 잘린 기록 수
    uint64 truncated = 0;
    // 런타임 템플릿 오류 기록 수
    uint64 templateErrors = 0;
    // 자리 서식 실패가 있은 기록 수
    uint64 formatErrors = 0;
    // sink 실패 횟수와 막은 횟수
    uint64 sinkFailures = 0;
    uint64 sinkBlocks = 0;
    // 출력 스레드·sink 콜백·중첩 준비 버퍼 깊이 초과에서 들어와 비상 경로로 보낸 기록 수
    uint64 reentered = 0;
    // 설치 전·해제 뒤 버린 낮은 레벨 기록 수(설치 때 받은 값)
    uint64 droppedBeforeInstall = 0;
    // 카테고리별 rate limit으로 버린 기록 수(모든 칸 합). 알림은 접두사마다 묶음 끝·Pump·Stop에서
    // 1초에 한 번까지 나간다.
    uint64 rateLimited = 0;
    // 출력 스레드를 만들지 못해 동기 비상 모드로 낮췄는지
    bool emergencyMode = false;
    // Stop이 멈춘 sink 때문에 출력 스레드를 회수하지 못하고 남겨 두었는지(Stop 설명)
    bool writerAbandoned = false;
};

class LogSystem;

namespace detail {
struct LogSystemAccess;

// 크래시 수집기의 종료 콜백(크래시 처리 스레드)이 부른다. 설치된 LogSystem을 lock 없이 찾아
// LogSystem::FlushForTermination으로 비운다(Docs/Design/CrashHandlingAndLaunch.ko.md 19절).
// - 기다리는 상한은 "공용 마감 - TerminationCallbackReserveMilliseconds"다. 콜백은 등록 역순으로
//   불리고(로그 콜백은 Core가 먼저 등록하므로 나중에 불린다), 남긴 여유는 로그 뒤에 불리는 콜백의
//   몫이다.
// - Log::Fatal·assert가 실패 스레드에서 flush를 시도했으면(MarkFatalFlushAttempted) 그 시도는 그
//   스레드의 제한 시간에 끊겼을 수 있다. 그래서 위 상한까지 남은 큐를 한 번 더 기다린다. 표시가
//   없으면 fatalFlushTimeoutMilliseconds와 위 상한 가운데 짧은 쪽까지 기다린다.
// - 결과는 FlushForTermination과 같다(상한이 지났으면 timeout 0으로 부른다). 설치된 LogSystem이
//   없으면 Skipped다.
inline constexpr uint32 TerminationCallbackReserveMilliseconds = 250;

platform::TerminationCallbackResult
FlushInstalledLogForTermination(const platform::TerminationContext& context) noexcept;

// Log::Fatal·assert가 실패 스레드에서 flush를 시도했다는 프로세스 표시다(스레드별 fatalInProgress와
// 별개, 크래시 처리 스레드가 읽는다).
void MarkFatalFlushAttempted() noexcept;
bool FatalFlushAttempted() noexcept;

// 설치·해제 때마다 하나씩 오르는 전역 세대 값이다(SRCU 세대 뒤집기). 해제가 뒤집기를 마쳤는지
// 순서를 확인하는 시험·진단용이다.
uint64 LogInstallGeneration() noexcept;
} // namespace detail

// 로그 기록 경로다. 호스트(main·시험)가 소유하고 LogInstallScope로 전역 대상에 설치한다.
//
// 스레드 계약:
// - Start·Stop·소멸은 소유 스레드 하나에서만, 서로 겹치지 않게 부른다.
// - AddSink·RemoveSink·SetGlobalMinimum·SetLevel·ClearLevel·SetFrame·SetThreadName·Flush·Stats는
//   어느 스레드에서나 부를 수 있다. sink 콜백 안에서 AddSink·RemoveSink·SetGlobalMinimum·SetLevel·
//   ClearLevel을 부르면 Busy, Flush는 false다.
// - lock 순서: 출력 쪽은 sink lock을 쥔 채 sink 콜백을 부른다. 그래서 sink 콜백이 잡는 lock을 쥔
//   채로 AddSink·RemoveSink·(Sync 모드의) Flush를 부르지 않는다(교착). 로그 호출(Log::*)은 어떤
//   lock 안에서도 된다(생산자는 sink lock을 잡지 않는다. Sync는 호출 스레드가 sink lock을 잡으므로
//   sink 콜백이 잡는 lock 안에서 Sync 로그를 남기지 않는다).
// - Pump는 Manual 모드에서 어느 스레드에서나 부를 수 있지만 한 번에 하나만 돈다(겹친 호출은 0).
class LogSystem {
public:
    LogSystem(Allocator& allocator, const LogSystemConfig& config) noexcept;
    ~LogSystem() noexcept;
    LogSystem(const LogSystem&) = delete;
    LogSystem& operator=(const LogSystem&) = delete;

    // 큐·블록 풀·렌더 버퍼를 확보하고(Async는) 출력 스레드를 시작한다. Stop 뒤 다시 시작할 수 있다.
    // - 실패: 이미 시작함·잘못된 설정은 InvalidArgument, 앞 Stop이 남겨 둔 출력 스레드가 아직 sink
    //   안에 있으면 Busy, 그 밖은 allocator 오류(아무것도 남기지 않는다).
    // - 출력 스레드를 만들지 못하면 Start는 성공하고 동기 비상 모드로 낮춘다: Warning 이상만 호출
    //   스레드에서 비상 출력(stderr 직접 쓰기)으로 쓰고 나머지는 버린다. 한 번 알린다.
    Result<void> Start() noexcept;

    // 새 기록을 받지 않고, 남은 기록을 비우고(Manual은 Stop한 스레드가 모두 비운다), sink를
    // flush하고, 출력 스레드를 회수한 뒤 메모리를 돌려준다. 설치된 상태면 먼저 해제해야 한다(설치
    // 범위가 이 객체보다 먼저 끝나야 한다). 두 번 불러도 된다.
    //
    // Async의 drain은 drainTimeoutMilliseconds 안의 best effort다. 넘기면 남은 기록을 버리기로
    // 하고(출력 스레드는 다음 기록에서 멈춘다) 버린 수를 비상 경로로 알린 뒤 같은 시간만큼 한 번 더
    // 기다린다. 그래도 출력 스레드가 sink 안에서 돌아오지 않으면(멈춘 sink) 기다리지 않고 반환한다:
    // 출력 스레드를 남겨 두고 그 스레드가 쓰는 큐·블록·렌더 메모리를 돌려주지 않는다(살아 있는
    // 스레드 아래에서 해제하지 않으려고 의도적으로 남긴다). 이때 Stats().writerAbandoned가 참이고,
    // 소멸자는 그 스레드가 sink에서 돌아올 때까지 기다린 뒤 회수한다. 멈춘 sink가 끝내 돌아오지
    // 않을 수 있으면 호스트는 LogSystem을 파괴하지 말고 프로세스를 끝낸다. writerAbandoned이면
    // 등록한 sink들도 LogSystem보다 오래 살아야 한다(남은 스레드가 돌아오는 순간 sink lock을 쥐고
    // 있다). 그동안 AddSink·RemoveSink는 Busy다. 출력 스레드는 sink에서 돌아온 뒤 다른 sink를
    // 부르지 않는다. sink 콜백 안에서 부르면 아무것도 하지 않는다(자기 자신을 기다리게 된다).
    void Stop() noexcept;

    // sink를 등록한다. 등록 뒤 시작하는 묶음부터 받는다. filter는 복사한다.
    // 실패: 중복 AlreadyExists, 용량 초과 CapacityExceeded, 잘못된 레벨 InvalidArgument, sink 콜백
    // 안에서 부르면 Busy.
    // 수명: sink는 RemoveSink가 성공하거나 이 LogSystem이 파괴될 때까지 살아 있어야 한다.
    // Stop만으로는 안 된다(소멸자가 다시 flush한다). 지역 변수면 sink를 LogSystem보다 먼저
    // 선언한다. 등록된 채 파괴되면 sink 소멸자가 Fatal이다(RuntimeLogSink 참고).
    Result<void> AddSink(RuntimeLogSink& sink, LogFilter filter = {}) noexcept;

    // sink를 뺀다. 반환되면 그 sink의 콜백은 실행 중이 아니고 다시 불리지 않는다(출력 쪽이 묶음을
    // 끝내고 sink lock을 놓을 때까지 기다린다). 미등록은 NotFound, sink 콜백 안에서 부르면 Busy.
    Result<void> RemoveSink(RuntimeLogSink& sink) noexcept;

    // Manual 모드: 큐에서 최대 maxRecords건을 꺼내 sink에 넘기고 처리한 수를 돌려준다. 다른 모드나
    // 다른 스레드가 Pump 중이면 0이다.
    usize Pump(usize maxRecords = MaxSize) noexcept;

    // 이 호출 전에 넣은 기록이 모두 sink에 도달할 때까지 기다린다(이벤트로 보장, 주기 타이머 없음).
    // timeout 안에 끝나면 true다. Sync는 sink flush만, Manual은 Pump로 비운다. sink 콜백 안·출력
    // 스레드에서 부르면 false다.
    bool Flush(uint32 timeoutMilliseconds = platform::InfiniteTimeout) noexcept;

    // 전역 최소 레벨이다. 컴파일에서 뺀 레벨보다 낮게 주면 성공하되 Warning을 한 번 남긴다.
    // 실패: 잘못된 레벨 InvalidArgument, sink 콜백 안에서 부르면 Busy.
    Result<void> SetGlobalMinimum(LogLevel level) noexcept;

    // 카테고리 이름 접두사(prefix)의 레벨을 재정의한다(점 접두사 최장 일치, ASCII 대소문자 무시).
    // 재정의는 전역보다 자세하게도 조용하게도 할 수 있다.
    // 실패: 이름 문법·레벨 오류 InvalidArgument, 표가 가득 차면 CapacityExceeded, 해시가 같은 다른
    // 이름이 이미 있으면 AlreadyExists, sink 콜백 안에서 부르면 Busy.
    Result<void> SetLevel(StringView prefix, LogLevel level) noexcept;

    // 재정의를 지운다. 없으면 NotFound, sink 콜백 안에서 부르면 Busy다.
    Result<void> ClearLevel(StringView prefix) noexcept;

    // 카테고리 이름 접두사(prefix)의 초당 기록 수 상한을 정한다(점 접두사 최장 일치, ASCII 대소문자
    // 무시). 1초 고정 창이고 창마다 통과 수가 정확히 perSecond 이하다. Trace–Warning에만 적용하고
    // Error·Fatal은 세지도 버리지도 않는다. 버린 수는 출력 쪽이 접두사마다 1초에 한 번까지
    // "Rate limit dropped ..." Info로 알린다.
    // 실패: 이름 문법·범위(MinLogRateLimitPerSecond–MaxLogRateLimitPerSecond) InvalidArgument, 표가
    // 가득 차면 CapacityExceeded, 해시가 같은 다른 이름이 있으면 AlreadyExists, sink 콜백 안 Busy.
    Result<void> SetRateLimit(StringView prefix, uint32 perSecond) noexcept;

    // prefix의 rate limit을 지운다. 없으면 NotFound, sink 콜백 안에서 부르면 Busy다.
    Result<void> ClearRateLimit(StringView prefix) noexcept;

    // rate limit을 모두 지운다. sink 콜백 안에서 부르면 Busy다.
    Result<void> ClearRateLimits() noexcept;

    // level·category의 기록이 지금 활성인지다(컴파일 타임 하한은 Log::IsEnabled가 본다). Fatal은 늘
    // 참이다. 잠금 없이 원자 변수만 읽는다.
    bool IsEnabled(LogLevel level, const LogCategory& category) const noexcept;

    // 엔진 프레임 번호다. 이후 기록의 LogEvent::frame에 실린다.
    void SetFrame(uint64 frame) noexcept;

    // 호출 스레드의 이름을 출력용 표에 등록한다(복사한다). 빈 이름은 지운다.
    // 실패: 잘못된 UTF-8·MaxLogThreadNameBytes 초과 InvalidArgument, 표가 가득 차면
    // CapacityExceeded.
    Result<void> SetThreadName(StringView name) noexcept;

    LogSystemStats Stats() const noexcept;

    // 비정상 종료 직전의 best effort flush다(크래시 수집기의 종료 콜백이 크래시 처리 스레드에서
    // 부른다, Docs/Design/CrashHandlingAndLaunch.ko.md 19절). 막히는 lock을 쓰지 않는다. Async는
    // 출력 스레드를 TrySignal로 깨우고, 이 호출 전에 넣은 기록이 sink flush까지 끝났는지 원자
    // 위치로 확인하며 timeout까지 기다린다. Sync는 sink lock을 TryLock으로 잡을 때만 flush한다.
    // failingThreadId(크래시한 스레드, 없으면 0)와 stoppedThreadIds(겹쳐 멈춘 스레드)는 출력
    // 스레드가 실패했는지 판정하는 데 쓴다. 출력 스레드가 실패했으면 큐를 비울 주체가 없으므로
    // 기다리지 않고 비상 경로로 알린다. 결과:
    // - Skipped: 시작 전, Manual, 비상 모드, sink 콜백 안, 출력 스레드 자신이 부름, 출력 스레드가
    //   실패·멈춘 스레드에 있음, Sync에서 sink lock을 잡지 못함
    // - TimedOut: Async에서 timeout까지 다 비우지 못함
    // - Completed: 다 비움
    platform::TerminationCallbackResult
    FlushForTermination(uint32 timeoutMilliseconds, uint64 failingThreadId,
                        Span<const uint64> stoppedThreadIds) noexcept;

    LogMode Mode() const noexcept {
        return config_.mode;
    }

private:
    friend struct detail::LogSystemAccess;
    // 출력 스레드가 크래시한 스레드(failing)나 겹쳐 멈춘 스레드(stopped)에 있는지다. 종료 flush의
    // 두 경로(콜백, FlushForTermination)가 같은 판정을 쓴다.
    bool WriterThreadFailed(uint64 failingThreadId,
                            Span<const uint64> stoppedThreadIds) const noexcept;
    friend platform::TerminationCallbackResult
    detail::FlushInstalledLogForTermination(const platform::TerminationContext& context) noexcept;
    friend class LogInstallScope;

    struct SinkSlot {
        RuntimeLogSink* sink = nullptr;
        LogFilter filter{};
        uint32 consecutiveFailures = 0;
        uint32 batchesSinceBlocked = 0;
        bool blocked = false;
        // 막혔다고 이미 알렸는지다(다시 시도해 실패해도 또 알리지 않는다. 성공하면 지운다).
        bool blockAnnounced = false;
    };

    struct QueueSlot {
        uint32 bytes = 0;
        uint8 data[LogSlotBytes];
    };

    struct LevelOverride {
        uint64 hash = 0;
        uint8 level = 0;
    };

    // rate limit 칸이다. 생산자는 원자 변수만 읽는다. state는 (창 번호 << 32) | 창 안 통과 수다.
    struct RateLimit {
        uint64 hash = 0;
        uint32 limit = 0;
        uint64 state = 0;
        uint64 dropped = 0;
        // 출력 쪽만 쓴다: 마지막으로 알린 버림 누계와 알린 초
        uint64 reported = 0;
        uint64 noticeSecond = 0;
        bool noticed = false;
    };

    struct ThreadName {
        uint64 threadId = 0;
        uint8 bytes = 0;
        char name[MaxLogThreadNameBytes + 1]{};
    };

    static void WriterEntry(void* context) noexcept;
    void WriterLoop() noexcept;
    usize DrainBatch(usize limit) noexcept;
    // Pump의 본체다. started_를 보지 않으므로 Stop의 Manual drain도 쓴다(pumping_은 호출자가 쥔다).
    usize DrainQueue(usize maxRecords) noexcept;
    void ReleaseResources() noexcept;
    // UTC 기준점을 다시 맞춘다(sinkMutex_ 안에서 부른다).
    void RefreshUtcAnchor() noexcept;
    void Dispatch(Span<const uint8> capture, const LogBlockProvider* blocks) noexcept;
    void FlushSinks() noexcept;
    void ReportDrops() noexcept;
    void Wake() noexcept;
    void Submit(LogLevel level, const LogCategory& category, const detail::LogCaptureTemplate& text,
                const detail::FormatArg* args, usize count) noexcept;
    void DeliverFatal(Span<const uint8> capture) noexcept;
    void CountDrop(LogLevel level) noexcept;
    // 기록 시각(주입 시계 또는 platform::MonotonicTicks)이다.
    uint64 Now() const noexcept;
    // level·category 기록이 rate limit을 통과하는지다(통과 못 하면 칸의 버림 수를 올린다).
    bool PassRateLimit(LogLevel level, const LogCategory& category, uint64 now) noexcept;
    // 칸별 버림 증가분을 Info로 알린다(출력 쪽, sinkMutex_ 안).
    void ReportRateLimits() noexcept;
    void WarnOnce(uint32& flag, StringView message) noexcept;
    uint8 EffectiveMinimum() const noexcept;
    StringView FindThreadName(uint64 threadId) noexcept;

    Allocator* allocator_;
    LogSystemConfig config_;
    BoundedMpscQueue<QueueSlot> queue_;
    LogBlockPool blocks_;
    char* render_ = nullptr;
    LogProperty properties_[MaxLogArguments];
    LogProperty context_[MaxLogContextDepth];
    // 펼친 값("{@Name}")의 렌더 노드다(출력 쪽만 쓴다).
    LogProperty structureNodes_[MaxLogStructureNodes];
    platform::Thread writer_;
    platform::ManualResetEvent wake_;

    // 시작·정지 상태(원자 변수)
    uint32 started_ = 0;
    uint32 accepting_ = 0;
    uint32 stopping_ = 0;
    uint32 emergency_ = 0;
    uint32 sleeping_ = 0;
    uint32 pumping_ = 0;
    // Stop이 drain을 포기했다(출력 스레드는 다음 기록에서 멈춘다).
    uint32 abandoned_ = 0;
    // Stop이 출력 스레드를 회수하지 못하고 메모리와 함께 남겨 두었다(소유 스레드만 읽고 쓴다).
    bool writerLeaked_ = false;
    uint64 writerThread_ = 0;
    // 출력 쪽이 sink flush까지 끝낸 큐 위치다(종료 flush가 lock 없이 읽는다).
    uint64 drainedPosition_ = 0;

    // sink 표(sinkMutex_가 보호). 출력 쪽은 묶음 동안 이 lock을 쥔다.
    platform::Mutex sinkMutex_;
    SinkSlot sinks_[MaxLogSinks];
    usize sinkCount_ = 0;

    // Flush 대기(묶음 끝에 완료 위치를 알린다)
    platform::Mutex flushMutex_;
    platform::ConditionVariable flushed_;
    uint64 completed_ = 0;

    // 레벨: 원자 변수만 잠금 없이 읽는다. 이름은 levelMutex_ 안에서만 읽고 쓴다.
    platform::Mutex levelMutex_;
    uint8 globalMinimum_;
    uint32 overrideCount_ = 0;
    uint32 overrideHighWater_ = 0;
    LevelOverride overrides_[LogLevelOverrideCapacity];
    char overrideNames_[LogLevelOverrideCapacity][MaxLogCategoryNameBytes + 1]{};
    uint32 warnedCompiledMinimum_ = 0;

    // rate limit 표(생산자는 원자 변수만 읽는다, 이름은 rateMutex_ 안)
    platform::Mutex rateMutex_;
    uint32 rateLimitCount_ = 0;
    uint32 rateLimitHighWater_ = 0;
    RateLimit rateLimits_[MaxLogRateLimits];
    char rateLimitNames_[MaxLogRateLimits][MaxLogCategoryNameBytes + 1]{};

    // 기록 시각 시계(Start 전에 정해진다)
    uint64 (*clock_)() noexcept = nullptr;
    uint64 clockFrequency_ = 0;

    uint64 frame_ = 0;

    platform::Mutex nameMutex_;
    ThreadName threadNames_[LogThreadNameCapacity];

    // 출력 쪽만 쓰는 UTC 기준점
    uint64 anchorMonotonic_ = 0;
    int64 anchorUtc_ = 0;
    bool anchorValid_ = false;
    // 마지막으로 알린 레벨별 버림 수다(알림은 그 뒤로 늘어난 만큼만 싣는다). dropped_는
    // Stop·Start를 거쳐도 누적이므로 다시 시작해도 기준을 유지한다.
    uint64 reportedDropped_[LogLevelCount]{};

    // 통계(원자 변수)
    uint64 dropped_[LogLevelCount]{};
    uint64 written_ = 0;
    uint64 truncated_ = 0;
    uint64 templateErrors_ = 0;
    uint64 formatErrors_ = 0;
    uint64 sinkFailures_ = 0;
    uint64 sinkBlocks_ = 0;
    uint64 reentered_ = 0;
    uint64 droppedBeforeInstall_ = 0;
};

// LogSystem을 범위 동안 전역 로그 대상으로 설치한다. 끝나면 이전 대상을 복원한다(중첩 가능,
// 역순으로 끝나야 한다). 해제가 반환되면 그 LogSystem을 쓰는 진행 중 호출이 없으므로 LogSystem과
// sink를 파괴해도 된다. 진행 중 호출 추적은 두 세대 전역 카운터(SRCU 방식)라 계속 로그가 들어와도
// 해제가 굶지 않는다.
//
// 설치 때 그 전(설치 전·해제 뒤)에 버린 낮은 레벨 기록 수를 한 번 알린다(Warning).
//
// 사용 예:
//   LogInstallScope installed(logs);
class LogInstallScope {
public:
    explicit LogInstallScope(LogSystem& system) noexcept;
    ~LogInstallScope() noexcept;
    LogInstallScope(const LogInstallScope&) = delete;
    LogInstallScope& operator=(const LogInstallScope&) = delete;

private:
    LogSystem* previous_;
};

} // namespace rocks
