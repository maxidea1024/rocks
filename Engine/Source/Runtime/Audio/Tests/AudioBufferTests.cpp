#include "AudioTestSupport.h"

#include <Base/InitializerList.h>
#include <Audio/AudioFormat.h>
#include <Audio/GainRamp.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace audio_buffer_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::audiotest;

// Audio DSP1의 형식·planar 버퍼·gain ramp
// 시험이다(Docs/Design/AudioSystem 3.20.1–3.20.3절, 3.20.8절).

TEST(Audio_DSP1_ChannelLayoutCountsAndOrder) {
    CHECK(ChannelCount(ChannelLayout::Mono) == 1);
    CHECK(ChannelCount(ChannelLayout::Stereo) == 2);
    CHECK(ChannelCount(ChannelLayout::Surround51) == 6);
    CHECK(ChannelCount(ChannelLayout::Surround71) == MaxOutputChannels);
    CHECK(ChannelAt(ChannelLayout::Mono, 0) == AudioChannel::FrontCenter);
    CHECK(ChannelAt(ChannelLayout::Stereo, 1) == AudioChannel::FrontRight);
    CHECK(ChannelAt(ChannelLayout::Surround51, 3) == AudioChannel::LowFrequency);
    CHECK(ChannelAt(ChannelLayout::Surround51, 4) == AudioChannel::SideLeft);
    CHECK(ChannelAt(ChannelLayout::Surround71, 6) == AudioChannel::BackLeft);
    CHECK(ChannelAt(ChannelLayout::Surround71, 7) == AudioChannel::BackRight);
    CHECK(IsValidMixSampleRate(DefaultMixSampleRate));
    CHECK(IsValidMixSampleRate(MinMixSampleRate) && IsValidMixSampleRate(MaxMixSampleRate));
    CHECK(!IsValidMixSampleRate(MinMixSampleRate - 1) &&
          !IsValidMixSampleRate(MaxMixSampleRate + 1));
}

TEST(Audio_DSP1_PlanarBufferAlignmentAndZeroFill) {
    SystemAllocator allocator;
    for (uint32 channels = 1; channels <= MaxOutputChannels; ++channels) {
        for (uint32 frames : {1u, 15u, 16u, 17u, 100u, MaxBlockFrames}) {
            PlanarBuffer buffer = MakeBuffer(channels, frames, allocator);
            REQUIRE(buffer.ChannelCount() == channels && buffer.FrameCapacity() == frames);
            const PlanarView view = buffer.View();
            REQUIRE(view.channelCount == channels && view.frameCount == frames);
            for (uint32 channel = 0; channel < channels; ++channel) {
                CHECK(reinterpret_cast<uptr>(view.channels[channel]) % AudioBufferAlignment == 0);
                CHECK(view.channels[channel] == buffer.Channel(channel));
                if (channel > 0) {
                    CHECK(view.channels[channel] - view.channels[channel - 1] >=
                          static_cast<isize>(frames));
                }

                for (uint32 frame = 0; frame < frames; ++frame) {
                    CHECK(view.channels[channel][frame] == 0.0f);
                }
            }
        }
    }
}

TEST(Audio_DSP1_PlanarBufferRejectsInvalidArguments) {
    SystemAllocator allocator;
    CHECK(PlanarBuffer::TryCreate(0, 16, allocator).Error() == ErrorCode::InvalidArgument);
    CHECK(PlanarBuffer::TryCreate(MaxOutputChannels + 1, 16, allocator).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(PlanarBuffer::TryCreate(2, 0, allocator).Error() == ErrorCode::InvalidArgument);
    CHECK(PlanarBuffer::TryCreate(2, MaxBlockFrames + 1, allocator).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(PlanarBuffer::TryCreate(MaxOutputChannels, MaxBlockFrames, allocator));
}

TEST(Audio_DSP1_PlanarBufferAllocationFailureAndMove) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    allocator.FailOnAttempt(1);
    CHECK(PlanarBuffer::TryCreate(2, 64, allocator).Error() == ErrorCode::OutOfMemory);
    allocator.DisableFailure();

    PlanarBuffer first = MakeBuffer(2, 64, allocator);
    first.Channel(1)[3] = 0.5f;
    PlanarBuffer second(Move(first));
    CHECK(first.ChannelCount() == 0 && first.FrameCapacity() == 0);
    CHECK(second.ChannelCount() == 2 && second.Channel(1)[3] == 0.5f);

    PlanarBuffer third = MakeBuffer(1, 8, allocator);
    third = Move(second);
    CHECK(third.ChannelCount() == 2 && third.Channel(1)[3] == 0.5f);
    third.Clear();
    CHECK(third.Channel(1)[3] == 0.0f);
}

TEST(Audio_DSP1_PlanarViewSliceAndConstView) {
    SystemAllocator allocator;
    PlanarBuffer buffer = MakeBuffer(2, 32, allocator);
    const PlanarView slice = buffer.View().Slice(8, 4);
    CHECK(slice.frameCount == 4 && slice.channelCount == 2);
    CHECK(slice.channels[1] == buffer.Channel(1) + 8);
    const PlanarConstView constant = slice;
    CHECK(constant.channels[0] == buffer.Channel(0) + 8 && constant.frameCount == 4);
    const PlanarConstView inner = constant.Slice(1, 2);
    CHECK(inner.channels[1] == buffer.Channel(1) + 9 && inner.frameCount == 2);
    CHECK(buffer.View().Slice(32, 0).frameCount == 0);
}

TEST(Audio_DSP1_MixAddAndRampGainFormula) {
    SystemAllocator allocator;
    PlanarBuffer source = MakeBuffer(2, 4, allocator);
    PlanarBuffer destination = MakeBuffer(2, 4, allocator);
    Fill(source.View(), 2.0f);
    Fill(destination.View(), 1.0f);

    MixAdd(destination.View(), source.View(), 0.25f);
    for (uint32 frame = 0; frame < 4; ++frame) {
        CHECK(destination.Channel(0)[frame] == 1.5f && destination.Channel(1)[frame] == 1.5f);
    }

    // n번째 이득은 start + (end − start)·n/N: 0, 0.25, 0.5, 0.75(끝값 1에는 닿지 않는다).
    destination.Clear();
    MixAddRamp(destination.View(), source.View(), 0.0f, 1.0f);
    const float expected[] = {0.0f, 0.5f, 1.0f, 1.5f};
    for (uint32 channel = 0; channel < 2; ++channel) {
        for (uint32 frame = 0; frame < 4; ++frame) {
            CHECK(destination.Channel(channel)[frame] == expected[frame]);
        }
    }

    // 감소 ramp와 0 frame 보기.
    destination.Clear();
    MixAddRamp(destination.View(), source.View(), 1.0f, 0.0f);
    CHECK(destination.Channel(0)[0] == 2.0f && destination.Channel(0)[3] == 0.5f);
    MixAddRamp(destination.View(0), source.View(0), 0.0f, 1.0f);

    Scale(destination.View(), 2.0f);
    CHECK(destination.Channel(1)[0] == 4.0f);
    Clear(destination.View());
    CHECK(destination.Channel(1)[0] == 0.0f);
}

TEST(Audio_DSP1_GainRampReachesTargetExactly) {
    SystemAllocator allocator;
    PlanarBuffer buffer = MakeBuffer(2, 1024, allocator);
    Fill(buffer.View(), 1.0f);

    GainRamp ramp(0.3f);
    ramp.SetTarget(0.7f, 1000);
    CHECK(ramp.IsRamping() && ramp.RemainingFrames() == 1000 && ramp.Current() == 0.3f);
    ramp.Apply(buffer.View());
    CHECK(!ramp.IsRamping() && ramp.RemainingFrames() == 0);
    CHECK(ramp.Current() == 0.7f && ramp.Target() == 0.7f);
    CHECK(buffer.Channel(0)[0] == 0.3f);
    // 500번째 frame은 정확히 중간, ramp 뒤 frame은 정확히 목표다.
    CHECK(test::Near(buffer.Channel(1)[500], 0.5f, 1.0e-7f));
    CHECK(buffer.Channel(0)[1000] == 0.7f && buffer.Channel(1)[1023] == 0.7f);
    for (uint32 frame = 1; frame < 1000; ++frame) {
        CHECK(buffer.Channel(0)[frame] > buffer.Channel(0)[frame - 1]);
    }
}

TEST(Audio_DSP1_GainRampBlockSplitInvariant) {
    SystemAllocator allocator;
    RandomStream random(0xA0D1);
    PlanarBuffer whole = MakeBuffer(3, MaxBlockFrames, allocator);
    FillRandom(whole.View(), random);
    PlanarBuffer split = MakeBuffer(3, MaxBlockFrames, allocator);
    for (uint32 channel = 0; channel < 3; ++channel) {
        memcpy(split.Channel(channel), whole.Channel(channel), sizeof(float) * MaxBlockFrames);
    }

    GainRamp wholeRamp(1.0f);
    wholeRamp.SetTarget(0.125f, 3001);
    wholeRamp.Apply(whole.View());

    GainRamp splitRamp(1.0f);
    splitRamp.SetTarget(0.125f, 3001);
    uint32 offset = 0;
    while (offset < MaxBlockFrames) {
        uint32 frames = 1 + random.NextUInt32Below(700);
        if (frames > MaxBlockFrames - offset) {
            frames = MaxBlockFrames - offset;
        }

        splitRamp.Apply(split.View().Slice(offset, frames));
        offset += frames;
    }

    CHECK(BitEqual(whole.View(), split.View()));
    CHECK(splitRamp.Current() == wholeRamp.Current());
}

TEST(Audio_DSP1_GainRampZeroFramesAndRetarget) {
    GainRamp ramp(0.0f);
    ramp.SetTarget(0.5f, 0);
    CHECK(!ramp.IsRamping() && ramp.Current() == 0.5f);

    // 0 → 1을 200 frame에 가다 100 frame에서 다시 0으로: 새 ramp는 0.5에서 시작한다.
    ramp.Reset(0.0f);
    ramp.SetTarget(1.0f, 200);
    ramp.Advance(100);
    CHECK(ramp.Current() == 0.5f && ramp.RemainingFrames() == 100);
    ramp.SetTarget(0.0f, 100);
    CHECK(ramp.Current() == 0.5f && ramp.RemainingFrames() == 100);
    ramp.Advance(50);
    CHECK(ramp.Current() == 0.25f);
    ramp.Advance(1000);
    CHECK(ramp.Current() == 0.0f && !ramp.IsRamping());

    // 목표 1로 끝난 ramp는 뒤 구간을 바꾸지 않는다.
    SystemAllocator allocator;
    PlanarBuffer buffer = MakeBuffer(1, 8, allocator);
    Fill(buffer.View(), 0.75f);
    GainRamp unity(1.0f);
    unity.Apply(buffer.View());
    CHECK(buffer.Channel(0)[7] == 0.75f);
}

} // namespace audio_buffer_tests_file
