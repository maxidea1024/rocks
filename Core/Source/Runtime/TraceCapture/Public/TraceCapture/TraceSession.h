#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Module/HostServiceKey.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>
#include <TraceCapture/TraceChannel.h>
#include <TraceCapture/TraceEventType.h>

namespace rocks {

namespace detail {
struct TraceSessionState;
} // namespace detail

// 수집 통계 채널 "stats"다(Trace.Stats, TraceSessionSettings::statsIntervalPumps). 세션이 꺼진
// 상태로 등록한다.
ROCKS_TRACECAPTURE_API extern TraceChannel TraceStatsChannel;

// 스레드 블록과 블록 풀의 메모리 배치다(Docs/Design/TraceSystem.ko.md 4.1절). 블록 하나는 앞에서
// 뒤로 자라는 기록 영역과 블록 끝의 머리 정보로 이뤄진다. 풀은 PageBlocks개 블록을 한 페이지로 한
// 번에 확보한다. 시험·통계가 블록 수를 계산할 때 쓴다.
struct TraceBlockLayout {
    static constexpr usize BlockBytes = 4096;
    static constexpr usize HeaderBytes = 24;
    static constexpr usize PayloadBytes = BlockBytes - HeaderBytes;
    static constexpr usize PageBlocks = 16;
    static constexpr usize PageBytes = BlockBytes * PageBlocks;
};

// writer 구동 방식이다(8절 "writer 주기와 수동 펌프").
enum class TraceWriterMode : uint8 {
    // 전용 플랫폼 스레드가 writerPeriodMs마다 비운다.
    Thread,
    // writer 스레드를 만들지 않는다. TraceSession::Pump를 부른 스레드에서만 비운다(시험·헤드리스의
    // 결정적 모드). 펌프하지 않으면 블록이 회수되지 않으므로 풀 고갈을 결정적으로 재현할 수 있다.
    Manual,
};

// TraceSession::Start 설정이다. 괄호 안은 설정 계층의 이름이다(TR5에서 연결).
struct TraceSessionSettings {
    // 세션의 모든 메모리(블록 페이지·정의 버퍼·저장소·스레드 상태·writer 버퍼)를 할당한다. 여러
    // 스레드가 동시에 부르므로 스레드 안전한 할당기여야 하고 세션 Stop까지 살아 있어야 한다.
    // 필수다.
    Allocator* allocator = nullptr;
    // 블록 풀 상한(trace.pool.maxBytes). PageBytes 단위로 내림하며 한 페이지 이상이어야 한다. 시작
    // 때부터 적용한다.
    usize poolMaxBytes = 64 * 1024 * 1024;
    // 정의 저장소 상한(trace.definitions.maxBytes). 넘친 정의는 이미 붙은 출력에만 가고 저장소에는
    // 남지 않는다. 0이면 저장하지 않는다.
    usize definitionStoreMaxBytes = 8 * 1024 * 1024;
    // 정의 기록을 writer가 비울 때까지 담는 공유 버퍼의 크기다.
    // TraceFormat::MaxDefinitionRecordBytes 의 네 배 이상이어야 한다. 차면 새 정의 기록은 버려지고
    // 세어진다.
    usize definitionBufferBytes = 256 * 1024;
    // 세션 이름 표(CPU 구간·시간 띠 이름)의 칸 수다. 2의 거듭제곱이고 1024 이상이어야 한다. 칸의
    // 3/4이 차면 새 이름은 넣지 못하고 그 이름의 구간은 기록하지 않는다(통계 scopeNamesRejected).
    usize nameTableCapacity = 16 * 1024;
    TraceWriterMode writerMode = TraceWriterMode::Thread;
    // writer 스레드의 비우기 주기(trace.writer.periodMs, ms). Thread 모드에서 0이 아니어야 한다.
    uint32 writerPeriodMs = 16;
    // 최근 기록 고리의 byte 수(trace.recent.bytes)다. 스레드 기록 덩어리를 출력이 없어도 여기에
    // 담고, 넘치면 가장 오래된 덩어리부터 버린다. 스냅샷(WriteSnapshot)이 고리를 쓴다. 0이면 고리가
    // 없다. 0이 아니면 MinRecentBytes 이상이어야 한다.
    usize recentBytes = 4 * 1024 * 1024;
    // 블록 풀 사용량이 상한의 이 백분율 이상이면 경고 로그를 한 번 남긴다(trace.pool.warnPercent).
    // 다시 아래로 내려간 뒤 넘으면 또 남긴다. 0이면 경고하지 않는다. 100 이하여야 한다.
    uint32 poolWarningPercent = 80;
    // 채널 "stats"가 켜져 있으면 이 펌프 수마다 수집 통계 기록(Trace.Stats)을 낸다
    // (trace.stats.intervalPumps). 1 이상이어야 한다.
    uint32 statsIntervalPumps = 60;

    // 최근 기록 고리의 최소 크기다(가장 큰 덩어리 하나와 고리 칸 표시가 들어간다).
    static constexpr usize MinRecentBytes = 32 * 1024;
};

// 스냅샷 writer 잠금의 기본 제한 시간(ms)이다(8절 "스냅샷").
inline constexpr uint32 DefaultSnapshotLockTimeoutMs = 1000;

// WriteSnapshot 결과다.
struct TraceSnapshotInfo {
    // writer 잠금을 제한 시간 안에 얻어 정의 저장소와 고리까지 썼는지다. 거짓이면 머리·스키마(얻을
    // 수 있었으면)·동기 표지만 썼고 머리에 HeaderFlagSnapshotIncomplete가 켜져 있다.
    bool complete = false;
    // 쓴 byte 수다.
    uint64 bytes = 0;
    // 고리에서 옮긴 덩어리 수와, 앞 덩어리를 잃어 기록 경계를 찾을 때까지 뺀 덩어리 수다.
    uint64 recentChunks = 0;
    uint64 skippedChunks = 0;
    // 세션 시작부터 고리가 넘쳐 버린 덩어리·byte 누계다.
    uint64 droppedChunks = 0;
    uint64 droppedBytes = 0;
};

// 한 시점의 세션 통계다. 여러 원자 칸을 따로 읽으므로 칸 사이의 일관성은 보장하지 않는다.
struct TraceSessionStats {
    // 스레드가 쓰고 있거나 writer가 아직 회수하지 않은 블록의 byte 수와 그 최대값이다.
    uint64 poolBytesInUse = 0;
    uint64 poolBytesPeak = 0;
    // 확보한 페이지의 byte 수와 상한이다.
    uint64 poolBytesReserved = 0;
    uint64 poolBytesLimit = 0;
    // 블록을 받지 못했거나 크기 상한을 넘어 버린 스레드 기록 수(모든 스레드 합)다.
    uint64 lostRecords = 0;
    // 정의 버퍼가 차서 버린 정의 기록 수다.
    uint64 definitionRecordsDropped = 0;
    // 정의 저장소의 byte 수(압축된 덩어리)와, 저장소 상한 때문에 저장하지 못한 정의 기록 수다.
    uint64 definitionStoreBytes = 0;
    uint64 definitionRecordsNotStored = 0;
    // 출력에 보낸 byte 수(모든 출력 합)와 쓰기에 실패한 출력 수다.
    uint64 outputBytes = 0;
    uint64 outputFailures = 0;
    // 마지막 출력 실패의 원본 오류다. 파일 출력(열기·쓰기·닫기)은 파일 상태(OS 오류 포함), 콜백
    // 출력은 콜백이 돌려준 오류다. 실패가 없었으면 Success·None이다.
    platform::FileStatus lastOutputFileStatus;
    ErrorCode lastOutputCallbackError = ErrorCode::None;
    // writer 버퍼를 늘리지 못해 버린 덩어리 수다(정상 경로에서는 0이다).
    uint64 writerDroppedChunks = 0;
    // 세션에 등록된 trace 스레드 수(종료한 스레드 포함, trace 스레드 id는 재사용하지 않는다)다.
    uint32 threadCount = 0;
    // 스레드 상태 저장 칸 수다. 끝난 스레드의 칸은 writer가 다 비운 뒤 재사용하므로 동시에 살아
    // 있던 스레드 수의 최대값 근처에 머문다.
    uint32 threadSlots = 0;
    // 이름 표에 든 이름 수와, 표가 차서 넣지 못한 이름 요청 수다.
    uint64 scopeNames = 0;
    uint64 scopeNamesRejected = 0;
    // 최근 기록 고리의 크기·사용 byte·담긴 덩어리 수와, 넘쳐서 버린 덩어리·byte 누계다.
    uint64 recentCapacity = 0;
    uint64 recentBytes = 0;
    uint64 recentChunks = 0;
    uint64 recentDroppedChunks = 0;
    uint64 recentDroppedBytes = 0;
    // 풀 사용률 경고를 남긴 횟수다(poolWarningPercent).
    uint64 poolWarnings = 0;
    // 낸 수집 통계 기록 수다.
    uint64 statsRecords = 0;
};

// 출력 콜백이다. 파일과 같은 byte 열(머리 + 덩어리)을 순서대로 받는다. bytes는 호출 동안만
// 유효하다. writer(Thread 모드의 writer 스레드 또는 Pump를 부른 스레드)에서 불린다. 콜백 안에서
// 같은 세션의 API를 부르면 교착한다. 실패를 반환하면 그 출력은 실패 상태가 되어 더 받지 않는다.
using TraceOutputWrite = Result<void> (*)(void* context, Span<const uint8> bytes) noexcept;

// 붙인 출력의 식별 값이다. 0은 쓰지 않는다.
struct TraceOutputId {
    uint32 value = 0;
};

// 현재 세션의 serial이다(세션마다 1부터 늘어나는 번호, 세션이 없으면 0). 세션별로 한 번 써야 하는
// 정의(GPU queue 정의 등)를 다시 쓸지 정할 때 쓴다. 어느 스레드에서나 부를 수 있다(relaxed 읽기에
// 가까운 acquire 읽기 하나).
ROCKS_TRACECAPTURE_API uint64 TraceCurrentSessionSerial() noexcept;

// 한 실행의 trace 수집 소유자다(UE TraceLog 전역 상태 대응, Docs/Design/TraceSystem.ko.md 3절).
// 채널 표·이벤트 형 표·블록 풀·정의 버퍼와 저장소·출력·writer를 가진다.
//
// 전역 싱글턴이 아니라 엔진이나 시험이 소유한다. 실행 중인 세션은 프로세스에 하나뿐이며, 기록
// API(TraceWrite)는 Start가 원자적으로 공개한 현재 세션에 쓴다.
//
// 사용 예:
//   SystemAllocator allocator;
//   TraceSession session;
//   TraceSessionSettings settings;
//   settings.allocator = &allocator;
//   if (!session.Start(settings)) { ... }
//   session.RegisterChannel(FrameChannel, true);      // 모듈 등록 함수
//   session.RegisterEventType(FrameBeginType);
//   auto output = session.AttachFileOutput("Saved/Traces/run.rtrace");
//   ...                                               // 여러 스레드에서 TraceWrite
//   session.Stop();                                   // 마지막 비우기와 출력 닫기
//
// 스레드: 제어 API(Start·Stop·등록·출력·Pump)는 어느 스레드에서나 부를 수 있지만, Start·Stop은 다른
// 제어 호출과 겹치지 않게 소유자가 순서를 정한다. 기록(TraceWrite)은 Stop과 겹쳐도 된다(Stop이 진행
// 중인 기록이 끝나기를 기다린다).
// 수명: 소멸자는 실행 중이면 Stop한다. 할당기는 Stop이 끝날 때까지 살아 있어야 한다.
class ROCKS_TRACECAPTURE_API TraceSession {
public:
    TraceSession() noexcept = default;
    ~TraceSession() noexcept;

    TraceSession(const TraceSession&) = delete;
    TraceSession& operator=(const TraceSession&) = delete;

    // 블록 풀·정의 버퍼·정의 저장소·writer(Thread 모드면 writer 스레드)를 만들고 현재 세션으로
    // 공개한다. 세션 머리 정보(기준 tick·주파수·시작 UTC·세션 GUID·빌드 id)를 이때 정한다.
    // 실패: 이미 실행 중 AlreadyExists, 다른 세션이 실행 중 Busy, 설정 오류 InvalidArgument, 메모리
    // OutOfMemory, writer 스레드·GUID·압축 문맥 준비 실패 DependencyFailed. 실패하면 아무것도
    // 남기지 않는다.
    Result<void> Start(const TraceSessionSettings& settings) noexcept;

    // 계획 29.4절의 종료 순서를 따른다. 1) 현재 세션 공개를 거두고 모든 채널을 끈다(새 기록 입장
    // 차단) 2) 진행 중인 기록 호출이 끝나기를 기다린다(생산자 종료 확인) 3) 남은 손실 기록을 쓰고
    // 마지막으로 비운다 4) 출력을 닫는다 5) writer 스레드를 join한다 6) 풀과 세션 메모리를
    // 해제한다. 등록된 채널과 이벤트 형은 꺼지고 id가 없어진다. 실행 중이 아니면 아무것도 하지
    // 않는다.
    void Stop() noexcept;

    bool IsRunning() const noexcept {
        return state_ != nullptr;
    }

    // 지금까지 공개된 기록을 출력으로 비운다. 순서: 스키마 → 정의 기록 → 스레드 기록. 정의 기록을
    // 쓰는 중인 다른 스레드가 있으면 그 정의보다 기록이 먼저 나가지 않도록 스레드 기록을 다음
    // 펌프로 미룬다. 모두 내보낸 블록은 풀로 돌려준다. Thread 모드에서도 부를 수 있다(즉시 비우기).
    // 실패: 실행 중이 아니면 WrongContext. 출력 쓰기 실패는 그 출력만 실패 상태로 두고 성공을
    // 반환한다.
    Result<void> Pump() noexcept;

    // 채널을 이 세션에 등록하고 id를 발급한다. enabled가 켜짐 초기 상태다(읽기 전용 채널은 이
    // 상태로 고정된다). 이름·설명을 정의 기록(ChannelInfo)으로 남긴다. 실패: 실행 중 아님
    // WrongContext, 이름 문법 오류·null 문자열 InvalidArgument, 이미 등록된 객체나 같은 이름
    // AlreadyExists, 채널 수 상한(TraceFormat::MaxChannels) 또는 정의 버퍼가 가득 참
    // CapacityExceeded(다음 펌프 뒤 다시 시도할 수 있다).
    Result<void> RegisterChannel(TraceChannel& channel, bool enabled = false) noexcept;

    // 채널 등록을 푼다(모듈을 내리기 전). 끄고 id를 없앤다. 이 채널을 쓰는 이벤트 형이 등록되어
    // 있으면 Busy다. 등록되지 않은 채널은 NotFound다. 같은 이름 채널을 다시 등록하면 이 id를 다시
    // 쓴다(Docs/Design/TraceSystem.ko.md 5절).
    Result<void> UnregisterChannel(TraceChannel& channel) noexcept;

    // 켜짐을 바꾸고 정의 기록(ChannelState)을 남긴다. 읽기 전용 채널은 Unsupported, 이 세션에
    // 등록되지 않은 채널은 NotFound다. 이름판은 등록된 채널 중 이름이 정확히 같은 것을 찾는다.
    Result<void> SetChannelEnabled(TraceChannel& channel, bool enabled) noexcept;
    Result<void> SetChannelEnabled(StringView name, bool enabled) noexcept;

    // 이벤트 형을 등록하고 형 id를 발급한다. 이름·필드 표는 스키마로 복사되어 다음 펌프에 스키마
    // 스트림으로 나간다. 형 id는 재사용하지 않는다.
    // 실패: 실행 중 아님 WrongContext, 채널이 이 세션에 등록되지 않음 NotFound, 이름·필드 표 오류
    // (빈 이름, 필드 수 상한, 고정부 밖 offset, 가변 필드의 offset이 Variable이 아님, 같은 필드
    // 이름) InvalidArgument, 이미 등록된 객체 또는 같은 이름·같은 스키마 AlreadyExists, 같은
    // 이름·다른 스키마 TypeMismatch, 고정부가 블록에 들어가지 않거나 형 수 상한 CapacityExceeded.
    Result<void> RegisterEventType(TraceEventType& type) noexcept;

    // 이벤트 형 등록을 푼다. 스키마는 세션에 남아 늦게 붙은 출력도 해석할 수 있다. 등록되지 않은
    // 형은 NotFound다. 같은 이름·같은 스키마를 다시 등록하면 이 형 id와 스키마를 다시
    // 쓴다(Docs/Design/TraceSystem.ko.md 4절).
    Result<void> UnregisterEventType(TraceEventType& type) noexcept;

    // 파일 출력을 붙인다(CreateAlways, 확장자는 보통 .rtrace). 붙는 즉시 파일 머리, 지금까지 내보낸
    // 스키마 전체, 정의 저장소 전체를 쓰고, 그 뒤 펌프부터 새 덩어리를 받는다. 정의 저장소가
    // 넘쳤거나 정의 기록이 버려진 적이 있으면 머리에 HeaderFlagDefinitionsIncomplete를 켠다. 실패:
    // 실행 중 아님 WrongContext, 출력 수 상한 CapacityExceeded, 파일 열기·쓰기 실패
    // DependencyFailed, 메모리 OutOfMemory. path는 NUL로 끝나는 UTF-8이며 호출 동안만 빌린다.
    Result<TraceOutputId> AttachFileOutput(const char* path) noexcept;

    // 콜백 출력을 붙인다. 계약은 AttachFileOutput과 같고 byte를 write로 받는다. context는 Detach나
    // Stop까지 살아 있어야 한다. write가 null이면 InvalidArgument다.
    Result<TraceOutputId> AttachCallbackOutput(TraceOutputWrite write, void* context) noexcept;

    // 출력을 뗀다. 떼기 전에 한 번 펌프해 이 호출 전에 공개된 기록까지 보낸 뒤 닫는다. 모르는 id는
    // NotFound다. 파일 닫기 실패는 DependencyFailed이며 출력은 떼어진다.
    Result<void> DetachOutput(TraceOutputId output) noexcept;

    // 스냅샷을 쓴다(8절): 머리(HeaderFlagSnapshot) → 지금까지 낸 스키마 전체 → 정의 저장소 → 최근
    // 기록 고리 → 동기 표지(Trace.SnapshotSync). 먼저 한 번 펌프해 지금까지 공개된 기록을 고리에
    // 넣는다. writer 잠금은 lockTimeoutMs 안에서만 시도하고, 얻지 못하면 정의 저장소·고리를 빼고
    // "불완전" (HeaderFlagSnapshotIncomplete)으로 쓴다(실패가 아니다). 고리 앞쪽을 잃어 기록
    // 중간에서 시작하는 스레드 덩어리는 그 스레드의 다음 기록 경계 덩어리까지 뺀다. 붙은 출력과
    // 고리는 바꾸지 않는다. 파일판은 path를 CreateAlways로 쓰고, 콜백판은 byte 전체를 write 한
    // 번으로 넘긴다(write 안에서 같은 세션 API를 불러도 된다 — 잠금 밖에서 부른다). 실패: 실행 중
    // 아님 WrongContext, 빈 path·null write InvalidArgument, 메모리 OutOfMemory, 파일
    // 열기·쓰기·닫기 실패는 그 파일 상태의 오류 코드(FileErrorCode — 원본 상태는 통계
    // lastOutputFileStatus), write가 돌려준 오류.
    Result<TraceSnapshotInfo>
    WriteSnapshot(const char* path, uint32 lockTimeoutMs = DefaultSnapshotLockTimeoutMs) noexcept;
    Result<TraceSnapshotInfo>
    WriteSnapshot(TraceOutputWrite write, void* context,
                  uint32 lockTimeoutMs = DefaultSnapshotLockTimeoutMs) noexcept;

    // 실행 중이 아니면 모두 0이다.
    TraceSessionStats Stats() const noexcept;

    // 스트림 머리에 쓰는 기준 tick(Start 때의 platform::MonotonicTicks)과 주파수, 세션 GUID다. 실행
    // 중이 아니면 0이다.
    uint64 BaseTick() const noexcept;
    uint64 TickFrequency() const noexcept;
    Guid SessionGuid() const noexcept;

private:
    detail::TraceSessionState* state_ = nullptr;
};

} // namespace rocks

// 호스트 서비스다(Docs/Design/ModuleLoader.ko.md 3.12절). 엔진 호스트가 모듈 단계 실행 전에
// 등록하고 호스트 묶음을 내린 뒤 뗀다.
ROCKS_DECLARE_HOST_SERVICE(rocks::TraceSession, ROCKS_TRACECAPTURE_API)
