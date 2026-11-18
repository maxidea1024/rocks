#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <TraceAnalysis/TraceStreamDecoder.h>

// trace 분석 모델이다(Docs/Design/TraceSystem.ko.md 10절 TR4). 파일 byte를 읽어 스레드별 CPU 구간,
// 순서 번호로 병합한 task·카운터, 표지 수, 손실·잘림·정의 누락 요약을 만든다. 손상된 입력도 멈추지
// 않고 읽을 수 있는 데까지 모델을 만들고 요약에 표시한다(OB03).
namespace rocks {

// 짝지은 CPU 구간이다. 끝을 잃은 구간은 ended가 false이고 endTick은 0이다(길이를 지어내지 않는다).
struct TraceModelScope {
    uint32 nameId = 0;
    uint64 beginTick = 0;
    uint64 endTick = 0;
    uint32 depth = 0;
    bool ended = false;
};

struct TraceModelThread {
    uint32 threadId = 0;
    uint64 osThreadId = 0;
    int32 sortOrder = 0;
    StringView name;
    StringView group;
    // 시작 순서의 구간이다.
    Array<TraceModelScope> scopes;
    // 이 스레드 손실 기록의 손실 수 합과 손실 구간 수다.
    uint64 lostRecords = 0;
    uint64 lossRanges = 0;
    // 시작이 없는 끝(시작 배치를 잃었거나 출력이 중간에 붙음)과 끝 없는 구간 수다.
    uint64 orphanEnds = 0;
    uint64 unendedScopes = 0;
    TraceDecodeStatus status = TraceDecodeStatus::Complete;

    explicit TraceModelThread(Allocator& allocator) noexcept : scopes(allocator) {
    }
};

// task 하나다. tick 0은 그 사건이 기록에 없다는 뜻이다(채널이 나중에 켜졌거나 출력이 중간에
// 붙었다).
struct TraceModelTask {
    uint64 id = 0;
    uint64 createdTick = 0;
    uint64 readyTick = 0;
    uint64 startedTick = 0;
    uint64 finishedTick = 0;
    uint64 completedTick = 0;
    bool completed = false;
    bool cancelled = false;
    // 실패 완료의 오류 이름이다(성공·취소는 빈 문자열).
    StringView error;
};

// 의존 간선(prerequisite → task)이다.
struct TraceModelEdge {
    uint64 task = 0;
    uint64 prerequisite = 0;
};

struct TraceModelCounter {
    uint32 id = 0;
    StringView name;
    uint8 kind = 0;
    uint8 unit = 0;
    uint64 samples = 0;
    // 순서 번호가 가장 큰 값 기록의 값 bit(정수는 int64, 실수는 double)다.
    uint64 lastBits = 0;
};

// GPU queue다(Trace.GpuQueue 정의, RB7). 정의 없이 구간만 있으면 이름이 빈 queue로 둔다.
struct TraceModelGpuQueue {
    uint16 id = 0;
    StringView name;
    uint8 kind = 0;
    // GPU 시간을 쟀는지다. 거짓이면(timestamp·calibration 미지원) 그 queue의 구간은 기록되지
    // 않는다(추정값 없음).
    bool timing = false;
    // calibration 쌍 수와 그 가운데 가장 큰 오차 반경(ns)이다.
    uint64 calibrations = 0;
    uint64 maxUncertaintyNanoseconds = 0;
};

// GPU 구간이다. tick은 기록 전에 calibration 쌍으로 trace 시간축으로 옮긴 값이다.
struct TraceModelGpuScope {
    uint16 queue = 0;
    uint32 nameId = 0;
    uint32 depth = 0;
    uint64 frame = 0;
    uint64 beginTick = 0;
    uint64 endTick = 0;
};

// GPU 프레임 경계다(그 queue에서 프레임 시작·끝 timestamp).
struct TraceModelGpuFrame {
    uint16 queue = 0;
    uint64 frame = 0;
    uint64 beginTick = 0;
    uint64 endTick = 0;
};

// 로그 호출 지점의 정의(Trace.LogMessageSpec)다. 같은 spec id가 다시 나오면 처음 것을 쓴다.
struct TraceModelLogSpec {
    uint64 specId = 0;
    uint64 eventId = 0;
    uint32 line = 0;
    uint8 level = 0;
    StringView category;
    StringView templateText;
    StringView file;
    StringView function;
};

// 로그 기록 하나(Trace.LogMessage)다. 순서 번호 순으로 모은다. thread는 생산자 OS 스레드 id다
// (Trace.ThreadInfo의 osThreadId로 대응한다). 상관 id는 0이면 없음이다.
struct TraceModelLog {
    uint64 order = 0;
    uint64 tick = 0;
    uint64 thread = 0;
    uint64 frame = 0;
    uint64 specId = 0;
    uint64 eventId = 0;
    uint64 correlation[TraceStandardFormat::ContextValueCount]{};
    uint32 flags = 0;
    uint8 level = 0;
    bool truncated = false;
    StringView message;
    // spec 정의 대신 기록에 실린 카테고리·템플릿이다(중복 표가 찼을 때만, 평소에는 빈 값).
    StringView inlineCategory;
    StringView inlineTemplate;
};

// 채널별 손실 합이다.
struct TraceModelChannelLoss {
    uint16 channelId = 0;
    StringView name;
    uint64 lost = 0;
};

struct TraceModelSummary {
    // 파일이 덩어리 중간에서 끊겼다.
    bool truncated = false;
    // 출력이 붙기 전에 정의가 저장소에서 넘쳤거나 버려졌다(머리 플래그).
    bool definitionsIncomplete = false;
    // 출력이 세션 중간에 붙었다(출력 머리의 순서 번호가 0이 아니다). 그보다 작은 순서 번호의 기록은
    // 없을 수 있다("앞부분 없음").
    bool attachedMidSession = false;
    uint64 attachOrderNumber = 0;
    uint64 lostRecords = 0;
    uint64 lossRanges = 0;
    // 정의 버퍼가 차서 버린 정의 기록 수(Trace.DefinitionLoss의 마지막 누계)다.
    uint64 definitionsDropped = 0;
    // 이름 정의가 없는 구간 이름 id 수다.
    uint64 missingScopeNames = 0;
    // 기록 중간에서 끝난 스트림 수와 모르는 형 때문에 끝까지 읽지 못한 스트림 수다.
    uint64 truncatedStreams = 0;
    uint64 undecodableStreams = 0;
    uint64 orphanScopeEnds = 0;
    uint64 unendedScopes = 0;
    // attachOrderNumber 이상에서 빠진 순서 번호 수(손실로 버려진 순서 기록)다.
    uint64 orderGaps = 0;
    uint64 marks = 0;
    // 스냅샷 파일인지(머리 HeaderFlagSnapshot)와, writer 잠금을 얻지 못해 정의·고리 없이 쓴 불완전
    // 스냅샷인지다.
    bool snapshot = false;
    bool snapshotIncomplete = false;
    // 동기 표지(Trace.SnapshotSync)의 고리 넘침 누계와 기록 경계를 잃어 뺀 덩어리 수다.
    uint64 recentDroppedChunks = 0;
    uint64 recentDroppedBytes = 0;
    uint64 snapshotSkippedChunks = 0;
};

// 수집 통계 기록(Trace.Stats)의 요약이다. 표본이 없으면 모두 0이다.
struct TraceModelCaptureStats {
    uint64 samples = 0;
    // 가장 이른·늦은 표본의 tick과 그때의 출력 byte 누계다(대역폭 = 출력 byte 차 / 시간 차).
    uint64 firstTick = 0;
    uint64 lastTick = 0;
    uint64 firstOutputBytes = 0;
    uint64 lastOutputBytes = 0;
    // 표본 가운데 가장 큰 풀 최대 사용 byte와, 마지막 표본의 풀 상한·정의 저장소·손실 누계다.
    uint64 poolBytesPeak = 0;
    uint64 poolBytesLimit = 0;
    uint64 definitionStoreBytes = 0;
    uint64 lostRecords = 0;
    uint64 definitionRecordsDropped = 0;
    uint64 recentDroppedBytes = 0;
};

// 파일 하나의 분석 모델이다. 모델의 문자열 view는 모델이 가진 decoder byte를 가리키므로 모델보다
// 오래 쓰지 않는다. 한 객체는 한 스레드에서 만들고 읽는다.
class TraceModel {
public:
    explicit TraceModel(Allocator& allocator) noexcept;

    TraceModel(const TraceModel&) = delete;
    TraceModel& operator=(const TraceModel&) = delete;

    // 모델을 만든다. 손실·잘림·정의 누락은 실패가 아니라 Summary에 표시한다.
    // 실패: 파일 머리가 없거나 형식이 다르면 InvalidArgument, 메모리 부족 OutOfMemory. 한 번만
    // 부른다.
    Result<void> Build(Span<const uint8> bytes) noexcept;

    const TraceStreamDecoder& Decoder() const noexcept {
        return decoder_;
    }

    // trace 스레드 id 순이다.
    const Array<TraceModelThread>& Threads() const noexcept {
        return threads_;
    }

    // task id 순이다.
    const Array<TraceModelTask>& Tasks() const noexcept {
        return tasks_;
    }

    const Array<TraceModelEdge>& Edges() const noexcept {
        return edges_;
    }

    // 카운터 id 순이다.
    const Array<TraceModelCounter>& Counters() const noexcept {
        return counters_;
    }

    // 로그 기록이다(순서 번호 순).
    const Array<TraceModelLog>& Logs() const noexcept {
        return logs_;
    }

    // 로그 호출 지점 정의다(spec id 순).
    const Array<TraceModelLogSpec>& LogSpecs() const noexcept {
        return logSpecs_;
    }

    // spec id의 정의다(이진 탐색). 없으면 nullptr다.
    const TraceModelLogSpec* FindLogSpec(uint64 specId) const noexcept;

    // 기록의 카테고리다: spec 정의가 있으면 그것, 없으면 기록에 실린 값이다.
    StringView LogCategoryOf(const TraceModelLog& log) const noexcept;

    // 채널 id 순이다.
    const Array<TraceModelChannelLoss>& ChannelLosses() const noexcept {
        return channelLosses_;
    }

    const TraceModelCaptureStats& CaptureStats() const noexcept {
        return captureStats_;
    }

    // queue id 순이다.
    const Array<TraceModelGpuQueue>& GpuQueues() const noexcept {
        return gpuQueues_;
    }

    // queue·시작 tick 순이다(같으면 깊이 순).
    const Array<TraceModelGpuScope>& GpuScopes() const noexcept {
        return gpuScopes_;
    }

    // queue·프레임 번호 순이다.
    const Array<TraceModelGpuFrame>& GpuFrames() const noexcept {
        return gpuFrames_;
    }

    const TraceModelSummary& Summary() const noexcept {
        return summary_;
    }

    // 이름 id의 이름이다. 정의가 없으면 빈 view다.
    StringView ScopeName(uint32 id) const noexcept;

    // 임계 경로다. 가장 늦게 완료한 task에서 시작해, 그 task의 의존 가운데 가장 늦게 완료한 앞
    // task로 거슬러 올라간다(같으면 작은 id). 앞 → 뒤 순서다.
    Result<void> CriticalPath(Array<uint64>& out) const noexcept;

private:
    struct NameEntry {
        uint32 id;
        StringView name;
    };

    struct OrderedEvent;

    Result<void> ReadDefinitions() noexcept;
    Result<void> ReadThread(uint16 stream, Array<OrderedEvent>& ordered) noexcept;
    Result<void> MergeOrdered(Array<OrderedEvent>& ordered) noexcept;
    TraceModelThread* FindThread(uint32 threadId) noexcept;
    TraceModelTask* FindOrAddTask(uint64 id) noexcept;
    void AddChannelLoss(uint16 channel, uint64 count) noexcept;
    TraceModelGpuQueue* FindOrAddGpuQueue(uint16 id) noexcept;
    void AddCaptureStats(const DecodedRecord& record) noexcept;

    Allocator* allocator_;
    TraceStreamDecoder decoder_;
    Array<TraceModelThread> threads_;
    Array<TraceModelTask> tasks_;
    Array<TraceModelEdge> edges_;
    Array<TraceModelCounter> counters_;
    Array<TraceModelChannelLoss> channelLosses_;
    Array<TraceModelGpuQueue> gpuQueues_;
    Array<TraceModelGpuScope> gpuScopes_;
    Array<TraceModelGpuFrame> gpuFrames_;
    Array<TraceModelLogSpec> logSpecs_;
    Array<TraceModelLog> logs_;
    TraceModelCaptureStats captureStats_;
    Array<NameEntry> names_;
    TraceModelSummary summary_;
};

// 두 tick 차이를 ns로 바꾼다(정수 소수 버림, overflow 없음). frequency가 0이면 0이다.
uint64 TraceTicksToNanoseconds(uint64 ticks, uint64 frequency) noexcept;

} // namespace rocks
