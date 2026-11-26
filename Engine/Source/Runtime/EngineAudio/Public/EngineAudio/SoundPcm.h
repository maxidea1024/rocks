#pragma once

#include <Audio/AudioMixerTypes.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Result.h>

// 재생 경로가 쥐는 불변 PCM이다(Docs/Design/AudioSystem 3.35.3절). 제어 pipe·AudioRT는 Object를
// 만지지 않으므로 재생 요청은 SoundClip 객체 대신 이 데이터의 공유 참조를 넘긴다. 마지막 참조는
// 제어 쪽이 clip 등록을 푼 뒤(AudioRT ACK 뒤) 놓을 수 있고 그 스레드가 제어 pipe일 수 있으므로,
// allocator는 스레드 안전해야 한다(엔진 기본 할당자·SystemAllocator).
//
// 사용 예:
//   auto made = SoundPcm::TryAllocate(allocator, 1, frames, 48000);
//   Span<float> samples = made.Value()->MutableChannel(0);
//   ... samples를 채운다 ...
//   SharedPtr<const SoundPcm> pcm = Move(made.Value());   // 이 뒤로 바꾸지 않는다
namespace rocks {

class ROCKS_ENGINEAUDIO_API SoundPcm {
public:
    // channelCount·frameCount·sampleRate를 검사하지 않은 빈 객체다. TryAllocate를 쓴다.
    explicit SoundPcm(Allocator& allocator) noexcept;

    // 0으로 채운 planar PCM을 만든다. 채널 1–MaxClipChannels, frame 1 이상, rate
    // [MinClipSampleRate, MaxClipSampleRate] 밖이면 InvalidArgument, 할당 실패는 OutOfMemory다.
    // 돌려준 쓰기 가능 참조로 표본을 채운 뒤 const 참조로 바꿔 넘긴다(넘긴 뒤 바꾸면 재생 중인
    // AudioRT가 읽는 메모리를 바꾸는 것이다).
    [[nodiscard]] static Result<SharedPtr<SoundPcm>> TryAllocate(Allocator& allocator,
                                                                 uint32 channelCount,
                                                                 uint32 frameCount,
                                                                 uint32 sampleRate) noexcept;

    uint32 ChannelCount() const noexcept {
        return channelCount_;
    }

    uint32 FrameCount() const noexcept {
        return frameCount_;
    }

    uint32 SampleRate() const noexcept {
        return sampleRate_;
    }

    // 채널 하나의 표본(frameCount개)이다. index는 ChannelCount 미만이어야 한다(assert).
    Span<const float> Channel(uint32 index) const noexcept;
    Span<float> MutableChannel(uint32 index) noexcept;

    // 모든 표본이 유한한지다(믹서 RegisterClip 조건과 같다).
    bool IsFinite() const noexcept;

private:
    Array<float> samples_;
    uint32 channelCount_ = 0;
    uint32 frameCount_ = 0;
    uint32 sampleRate_ = 0;
};

} // namespace rocks
