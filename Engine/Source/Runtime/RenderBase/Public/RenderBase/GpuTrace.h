#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Graphics/Device.h>
#include <Platform/Sync.h>
#include <RenderBase/RenderNotices.h>
#include <TraceCapture/TraceCpuScope.h>
#include <TraceCapture/TraceSession.h>

// GPU 계측이다(Docs/Design/RenderBase 13절 RB7, Docs/Design/TraceSystem.ko.md 9절). 이름 구간
// 하나가 debug marker(캡처 도구)와 timestamp 한 쌍을 함께 기록하고, GPU 완료 뒤 완료 통지 큐로 받은
// timestamp를 calibration 쌍으로 trace tick에 옮겨 trace(채널 "gpu")에 쓴다.
namespace rocks::render {

// 채널 "gpu"다. RegisterGpuTraceTypes가 세션에 꺼진 상태로 등록한다.
ROCKS_RENDERBASE_API extern TraceChannel GpuTraceChannel;

// 채널 "gpu"와 GPU 기록 형(Trace.GpuQueue·Trace.GpuCalibration·Trace.GpuScope·Trace.GpuFrame,
// TraceStandardFormat)을 세션에 등록한다. RenderBase 모듈의 Startup이 호스트 서비스의 trace 세션에
// 부른다(Docs/Design/ModuleLoader 3.12절). 시험은 직접 부른다. 실패: TraceSession::
// RegisterChannel·RegisterEventType의 오류다. 실패하면 이 호출이 등록한 것을 되돌린다.
ROCKS_RENDERBASE_API Result<void> RegisterGpuTraceTypes(TraceSession& session) noexcept;

// RegisterGpuTraceTypes의 등록을 형부터, 채널은 마지막에 푼다(RenderBase 모듈 Shutdown). 등록되지
// 않은 것은 건너뛴다. 실패: TraceSession::UnregisterEventType·UnregisterChannel의 NotFound 밖 첫
// 오류다(나머지는 계속 푼다).
ROCKS_RENDERBASE_API Result<void> UnregisterGpuTraceTypes(TraceSession& session) noexcept;

// GPU tick을 calibration 쌍으로 trace tick(platform::MonotonicTicks, traceFrequency)으로 옮긴다.
// 기준보다 앞선 tick은 그만큼 앞선 trace tick이다(0 아래는 0). 1초 미만 부분만 double로 계산해 중간
// 곱이 넘치지 않는다. gpuFrequency가 0이면 cpuTicks를 돌려준다.
ROCKS_RENDERBASE_API uint64 GpuTicksToTraceTicks(const graphics::ClockCalibration& calibration,
                                                 uint64 gpuTicks, uint64 traceFrequency) noexcept;

struct GpuTraceSettings {
    // debug marker(PushMarker·PopMarker)를 기록할지다. Release에서는 끈다(13.2절).
    bool markers = !IsReleaseBuild;
    // timestamp로 GPU 구간을 잴지다. Release는 기본 꺼짐이고 설정으로 켠다(13.2절).
    bool timestamps = !IsReleaseBuild;
    // calibration 쌍을 다시 얻는 프레임 간격(1 이상)이다. 쌍은 정의 기록으로 남고, 그 사이 프레임은
    // 가장 최근 쌍으로 옮긴다(두 시계의 drift는 보정하지 않는다).
    uint32 calibrationIntervalFrames = 60;
    // queue 정의(Trace.GpuQueue)의 id와 이름이다. 이름은 정적 수명 문자열이다.
    uint16 queueId = 0;
    const char* queueName = "Graphics";
    // 이 장부가 맡는 장치 queue다(async compute queue 구간은 따로 장부 하나를 둔다, RenderBase 8절
    // RG4). calibration은 CalibrateClocks(queue)이고 Consume은 이 queue의 timestamp 통지만 쓴다.
    graphics::QueueKind queue = graphics::QueueKind::Graphics;
};

struct GpuTraceStats {
    // trace에 쓴 GPU 구간·프레임 수다.
    uint64 scopesRecorded = 0;
    uint64 framesRecorded = 0;
    // timestamp가 아직 오지 않은(GPU 미완료) 구간·프레임 수다.
    uint64 pending = 0;
    // 구간이 끊겼거나(disjoint) timestamp를 쓰지 못해 버린 구간 수다.
    uint64 invalid = 0;
    // 얻은 calibration 쌍 수다.
    uint64 calibrations = 0;
    // 마지막 프레임에 GPU 시간을 잴 수 있었는지다(채널·설정·장치 지원·calibration 모두).
    bool timing = false;
};

class GpuScope;

// 한 queue의 GPU 계측 장부다. 렌더 lane이 프레임을 기록하며 BeginFrame·GpuScope·EndFrame을 부르고,
// 완료 통지를 받는 쪽(보통 게임 lane)이 Consume으로 끝난 구간을 trace에 쓴다.
//
// 사용 예:
//   // 렌더 lane(프레임 packet 작업 안)
//   auto frame = device.BeginOffscreen(target, {});
//   gpuTrace.BeginFrame(device, *frame.Value(), context.FrameNumber());
//   {
//       GpuScope pass(gpuTrace, *frame.Value(), ShadowPassName);   // static TraceScopeName
//       ... 그리기 ...
//   }
//   gpuTrace.EndFrame(*frame.Value());
//   device.EndFrame();
//   // 게임 lane
//   lane.Notices().TakeAll(notices);
//   gpuTrace.Consume(notices.View());
//
// 규칙:
// - GPU 시간은 채널 "gpu"가 켜져 있고, 설정 timestamps가 켜져 있고, 장치가 timestamp와
//   calibration을 모두 지원할 때만 잰다. 아니면 queue 정의에 "GPU 시간 없음"을 공개하고 구간을
//   기록하지 않는다(추정값을 만들지 않는다). marker는 설정 markers만 따른다.
// - queue 정의는 세션마다 첫 BeginFrame에서, calibration 쌍은 세션 첫 프레임과
//   calibrationIntervalFrames마다 정의 기록으로 쓴다.
// - 구간 값은 GPU 완료 뒤의 timestamp 통지로만 정해진다. 완료 전에는 pending이고 trace에 나가지
//   않는다. disjoint 통지가 하나라도 있으면 그 구간은 버린다.
// - GPU 구간 이름은 CPU 구간과 같은 세션 이름 표(Trace.ScopeName)를 쓴다(같은 pass 이름은 같은 id).
//
// 스레드: BeginFrame·EndFrame·GpuScope는 장치를 소유한 스레드(렌더 lane)에서 부른다.
// Consume·Stats는 어느 스레드에서나 부를 수 있다(내부 잠금). 수명: 장치·allocator보다 먼저
// 파괴한다.
class ROCKS_RENDERBASE_API GpuTrace {
public:
    explicit GpuTrace(Allocator& allocator, const GpuTraceSettings& settings = {}) noexcept;

    GpuTrace(const GpuTrace&) = delete;
    GpuTrace& operator=(const GpuTrace&) = delete;

    // 프레임 기록을 시작한 직후 부른다. queue 정의·calibration(때가 되었으면)을 쓰고 프레임 시작
    // timestamp를 기록한다.
    void BeginFrame(graphics::Device& device, graphics::CommandContext& commands,
                    uint64 frame) noexcept;

    // 제출(Device::EndFrame) 직전에 부른다. 프레임 끝 timestamp를 기록한다.
    void EndFrame(graphics::CommandContext& commands) noexcept;

    // 완료 통지 가운데 GpuTimestamp를 받아, 두 끝이 모두 온 구간·프레임을 trace tick으로 옮겨 쓴다.
    // 다른 종류는 무시한다.
    void Consume(Span<const RenderNotice> notices) noexcept;

    GpuTraceStats Stats() const noexcept;

    // GpuScope의 RAII 없는 형태다(렌더 그래프 구간, RenderBase 8절 RG1 남은 것 설계 2). 시작과 끝을
    // 서로 다른 장치 기록에 쓸 수 있다(같은 queue의 제출 순서라 시간이 맞는다). marker는 쓰지
    // 않는다(marker는 기록 안에서 짝이 맞아야 하므로 호출자가 기록마다 낸다). 깊이는 BeginSpan마다
    // 오르고 EndSpan마다 내린다(돌려준 값이 0이어도 EndSpan을 짝으로 부른다). 스레드는 BeginFrame과
    // 같다.
    uint64 BeginSpan(graphics::CommandContext& commands, TraceScopeName& name) noexcept;
    void EndSpan(graphics::CommandContext& commands, uint64 span) noexcept;
    // 끝 timestamp를 쓸 장치 기록이 없을 때(기록 실패 뒤 정리) span을 무효로 닫는다.
    void DropSpan(uint64 span) noexcept;

    // 설정 markers다(구간 이름을 debug marker로 남기는지).
    bool MarkersEnabled() const noexcept {
        return settings_.markers;
    }

private:
    friend class GpuScope;

    struct Pending {
        uint64 id = 0;
        uint64 frame = 0;
        uint64 beginQuery = 0;
        uint64 endQuery = 0;
        uint64 beginTicks = 0;
        uint64 endTicks = 0;
        graphics::ClockCalibration calibration;
        uint32 nameId = 0;
        uint32 depth = 0;
        bool isFrame = false;
        bool haveBegin = false;
        bool haveEnd = false;
        bool invalid = false;
        // 끝 timestamp를 쓰기로 정했는지다(GpuScope 소멸·EndFrame).
        bool closed = false;
    };

    uint64 OpenScope(graphics::CommandContext& commands, TraceScopeName& name) noexcept;
    void CloseScope(graphics::CommandContext& commands, uint64 id) noexcept;
    Pending* Find(uint64 id) noexcept;
    void Emit(const Pending& entry) noexcept;
    void WriteQueueDefinition(bool timing) noexcept;
    void WriteCalibration(const graphics::ClockCalibration& calibration) noexcept;

    GpuTraceSettings settings_;
    mutable platform::Mutex gate_;
    Array<Pending> pending_;
    GpuTraceStats stats_;
    uint64 nextId_ = 1;
    // 아래는 렌더 lane만 쓴다.
    uint64 sessionSerial_ = 0;
    uint64 calibratedFrame_ = 0;
    bool haveCalibration_ = false;
    graphics::ClockCalibration calibration_;
    uint64 frame_ = 0;
    uint64 frameEntry_ = 0;
    uint32 depth_ = 0;
    bool timing_ = false;
};

// 이름 있는 GPU 구간이다. 생성 때 marker push와 시작 timestamp를, 소멸 때 끝 timestamp와 marker
// pop을 기록한다. 중첩할 수 있다(깊이가 기록에 남는다). GpuTrace::BeginFrame과 EndFrame 사이, 같은
// CommandContext로 쓴다. 스택 객체로 쓴다.
class ROCKS_RENDERBASE_API GpuScope {
public:
    GpuScope(GpuTrace& trace, graphics::CommandContext& commands, TraceScopeName& name) noexcept;
    ~GpuScope() noexcept;

    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;

private:
    GpuTrace& trace_;
    graphics::CommandContext& commands_;
    uint64 entry_ = 0;
};

} // namespace rocks::render
