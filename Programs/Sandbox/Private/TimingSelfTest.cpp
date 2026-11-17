#include "TimingSelfTest.h"

#include <Core/Containers/Array.h>
#include <Platform/Clock.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// offscreen 대상 크기와 slot 재사용 시험의 프레임 수(TimestampFrameSlots보다 많게)다.
constexpr uint32 TargetSize = 16;
constexpr uint32 RepeatFrames = TimestampFrameSlots + 2;
// calibration으로 옮긴 GPU 시각이 앞뒤 CPU 시각 밖으로 벗어나도 되는 기본 폭(ns)이다. 시계 변환의
// 반올림을 덮는다. 장치가 공개한 두 calibration의 오차 반경(uncertaintyNanoseconds)을 여기에
// 더한다(D3D11·OpenGL은 CPU 부하에 따라 반경이 커진다).
constexpr uint64 CalibrationSlackNanoseconds = 1000000;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s timing: %s\n", backendName, what);
    }
}

// 프레임 하나에 timestamp 한 쌍으로 감싼 그리기를 기록하고 GPU 완료까지 기다린다.
struct TimedFrame {
    uint64 begin = 0;
    uint64 end = 0;
    bool ok = false;
};

TimedFrame DrawTimed(Device& device, sample::Renderer& renderer, TextureHandle target) noexcept {
    TimedFrame result;
    auto frame = device.BeginOffscreen(target, {});
    if (!frame) {
        return result;
    }

    CommandContext& commands = *frame.Value();
    commands.PushMarker(StringView("timed draw"));
    auto begin = commands.WriteTimestamp();
    sample::RenderView view;
    view.viewProjection = Mat4::Identity();
    sample::RenderItem item;
    item.mesh = sample::MeshKind::Triangle;
    renderer.Draw(commands, view, Span<const sample::RenderItem>(&item, 1));
    auto end = commands.WriteTimestamp();
    commands.PopMarker();
    auto submitted = device.EndFrame();
    result.ok = begin && end && submitted && device.WaitFence(submitted.Value()).IsOk();
    result.begin = begin ? begin.Value() : 0;
    result.end = end ? end.Value() : 0;
    return result;
}

// 회수한 결과에서 query 번호의 결과를 찾는다.
const TimestampSample* Find(const Array<TimestampSample>& samples, uint64 query) noexcept {
    for (const TimestampSample& sample : samples) {
        if (sample.query == query) {
            return &sample;
        }
    }

    return nullptr;
}

// 지금 회수할 수 있는 결과를 모두 꺼낸다.
bool CollectAll(Device& device, Array<TimestampSample>& out) noexcept {
    TimestampSample batch[8];
    for (;;) {
        const usize count = device.CollectTimestamps(Span<TimestampSample>(batch));
        if (!out.TryAppend(Span<const TimestampSample>(batch, count))) {
            return false;
        }

        if (count < 8) {
            return true;
        }
    }
}

void RunUnsupported(Device& device, TextureHandle target) noexcept {
    Check(device.Info().timestampFrequency == 0 && !device.Info().clockCalibration,
          "unsupported timestamps publish no frequency and no calibration");
    Check(device.CalibrateClocks().Error() == ErrorCode::Unsupported,
          "calibration is refused without timestamps");
    auto frame = device.BeginOffscreen(target, {});
    if (frame) {
        Check(frame.Value()->WriteTimestamp().Error() == ErrorCode::Unsupported,
              "timestamp write is refused without support");
        auto ended = device.EndFrame();
        (void)ended;
    }

    TimestampSample sample;
    Check(device.CollectTimestamps(Span<TimestampSample>(&sample, 1)) == 0,
          "no timestamp values are invented");
    printf("  %s timing: timestamps none\n", backendName);
}

void RunPair(Device& device, sample::Renderer& renderer, TextureHandle target,
             Allocator& allocator) noexcept {
    auto before = device.CalibrateClocks();
    const TimedFrame frame = DrawTimed(device, renderer, target);
    auto after = device.CalibrateClocks();
    Array<TimestampSample> samples(allocator);
    Check(frame.ok && CollectAll(device, samples), "timed frame completes and is collected");
    const TimestampSample* begin = Find(samples, frame.begin);
    const TimestampSample* end = Find(samples, frame.end);
    Check(begin != nullptr && end != nullptr, "both timestamps arrive after GPU completion");
    if (begin == nullptr || end == nullptr) {
        return;
    }

    Check(!begin->disjoint && !end->disjoint, "timestamps are valid");
    Check(end->ticks >= begin->ticks && frame.end == frame.begin + 1,
          "end timestamp follows begin");
    if (!device.Info().clockCalibration) {
        // timestamp는 있지만 CPU 시간축과 잇는 쌍이 없는 장치다(Vulkan calibrated timestamps 확장
        // 없음). 추정값을 만들지 않고 거부해야 한다.
        Check(before.Error() == ErrorCode::Unsupported && after.Error() == ErrorCode::Unsupported,
              "calibration without support is refused");
        printf("  %s timing: %llu Hz, clock calibration none\n", backendName,
               static_cast<unsigned long long>(device.Info().timestampFrequency));
        return;
    }

    Check(before && after, "clock calibration pairs");
    if (!before || !after) {
        return;
    }

    // 프레임은 앞 calibration 뒤에 제출되고 뒤 calibration 전에 끝났다.
    const uint64 cpuBegin = GpuTicksToCpuNanoseconds(before.Value(), begin->ticks);
    const uint64 cpuEnd = GpuTicksToCpuNanoseconds(before.Value(), end->ticks);
    const uint64 slack = CalibrationSlackNanoseconds + before.Value().uncertaintyNanoseconds +
                         after.Value().uncertaintyNanoseconds;
    const bool inside = cpuBegin + slack >= before.Value().cpuNanoseconds &&
                        cpuEnd <= after.Value().cpuNanoseconds + slack;
    Check(inside, "GPU interval maps between the surrounding CPU times");
    Check(after.Value().cpuNanoseconds <= platform::MonotonicNanoseconds() &&
              after.Value().gpuTicks >= before.Value().gpuTicks,
          "calibration is on the monotonic CPU timeline");
    const uint64 gpuNanoseconds =
        TicksToNanoseconds(end->ticks - begin->ticks, device.Info().timestampFrequency);
    printf("  %s timing: draw %llu ns on GPU, %llu Hz\n", backendName,
           static_cast<unsigned long long>(gpuNanoseconds),
           static_cast<unsigned long long>(device.Info().timestampFrequency));
    if (!inside) {
        printf("    cpu %llu..%llu, gpu mapped %llu..%llu, uncertainty %llu/%llu ns\n",
               static_cast<unsigned long long>(before.Value().cpuNanoseconds),
               static_cast<unsigned long long>(after.Value().cpuNanoseconds),
               static_cast<unsigned long long>(cpuBegin), static_cast<unsigned long long>(cpuEnd),
               static_cast<unsigned long long>(before.Value().uncertaintyNanoseconds),
               static_cast<unsigned long long>(after.Value().uncertaintyNanoseconds));
    }
}

void RunSlotReuse(Device& device, sample::Renderer& renderer, TextureHandle target,
                  Allocator& allocator) noexcept {
    // 프레임마다 GPU 완료를 기다리므로 회수 전 slot이 없고, slot 수보다 많은 프레임이 모두 값을
    // 낸다.
    uint64 queries[RepeatFrames * 2] = {};
    bool ok = true;
    for (uint32 index = 0; index < RepeatFrames && ok; ++index) {
        const TimedFrame frame = DrawTimed(device, renderer, target);
        ok = frame.ok;
        queries[index * 2] = frame.begin;
        queries[index * 2 + 1] = frame.end;
    }

    Array<TimestampSample> samples(allocator);
    Check(ok && CollectAll(device, samples) && samples.Size() == RepeatFrames * 2,
          "every frame's timestamps return across slot reuse");
    bool ordered = samples.Size() == RepeatFrames * 2;
    for (usize index = 0; ordered && index < samples.Size(); ++index) {
        ordered = samples[index].query == queries[index] && !samples[index].disjoint;
    }

    Check(ordered, "timestamps return in write order");
}

void RunMarkers(Device& device, TextureHandle target) noexcept {
    const uint64 ignored = device.IgnoredDraws();
    auto frame = device.BeginOffscreen(target, {});
    if (!frame) {
        Check(false, "marker frame");
        return;
    }

    CommandContext& commands = *frame.Value();
    // 짝 없는 pop은 무시하고 계수를 올린다. 닫지 않은 구간은 프레임 끝에 장치가 닫는다.
    commands.PopMarker();
    commands.PushMarker(StringView("outer"));
    commands.PushMarker(StringView("inner"));
    commands.PopMarker();
    Check(device.EndFrame().IsOk() && device.WaitIdle().IsOk(), "marker frame submits");
    Check(device.IgnoredDraws() == ignored + 1, "unmatched marker pop is ignored and counted");
    Check(device.Info().debugMarkers, "debug markers are published");
}

} // namespace

uint32 RunTimingChecks(const char* name, Device& device, sample::Renderer& renderer,
                       Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    TextureDesc targetDesc;
    targetDesc.width = TargetSize;
    targetDesc.height = TargetSize;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    Check(target.IsOk(), "render target creation");
    if (!target) {
        return failures;
    }

    // 기록 밖에서 바로 얻은 CommandContext는 없으므로, 기록이 끝난 context로 쓰기를 거부하는지
    // 본다.
    auto frame = device.BeginOffscreen(target.Value(), {});
    CommandContext* stale = frame ? frame.Value() : nullptr;
    Check(frame && device.EndFrame().IsOk(), "empty frame");
    if (!device.Info().timestamps) {
        RunUnsupported(device, target.Value());
    } else {
        if (stale != nullptr) {
            Check(stale->WriteTimestamp().Error() == ErrorCode::InvalidArgument,
                  "timestamp write outside recording is refused");
        }

        RunPair(device, renderer, target.Value(), allocator);
        RunSlotReuse(device, renderer, target.Value(), allocator);
    }

    RunMarkers(device, target.Value());
    device.Retire(target.Value());
    Check(device.WaitIdle().IsOk(), "timing checks finish idle");
    return failures;
}

} // namespace sandbox
