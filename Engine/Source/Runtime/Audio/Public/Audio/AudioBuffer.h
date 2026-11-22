#pragma once

#include <Audio/AudioFormat.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

// planar float32 오디오 버퍼와 그 보기다(Docs/Design/AudioSystem.ko.md 3.20.2절).
//
// 믹서 내부의 모든 PCM은 채널마다 따로 이어진 float32 배열(planar)이다. PlanarBuffer가 메모리를
// 소유하고, 처리 함수는 소유하지 않는 PlanarView·PlanarConstView를 받는다. 보기는 채널 포인터를
// 값으로 들고 있으므로(최대 MaxOutputChannels개) 복사해도 버퍼를 다시 가리킬 뿐이다. 보기의 수명은
// 버퍼보다 짧아야 한다.
//
// 사용 예:
//   auto created = PlanarBuffer::TryCreate(2, 512, allocator);
//   if (!created) { return created.Error(); }
//   PlanarBuffer& mix = created.Value();
//   mix.Clear();
//   MixAddRamp(mix.View(256), source, 0.0f, 1.0f);
namespace rocks::audio {

// 채널마다의 시작 주소 정렬(byte)이다. SIMD 적재와 캐시 줄에 맞춘다.
inline constexpr usize AudioBufferAlignment = 64;

// 쓰기 가능한 planar 보기다. channels[0..channelCount)만 의미가 있다.
struct PlanarView {
    float* channels[MaxOutputChannels] = {};
    uint32 channelCount = 0;
    uint32 frameCount = 0;

    float* Channel(uint32 index) const noexcept {
        return channels[index];
    }

    // [frameOffset, frameOffset + frames) 구간의 보기다. 범위는 호출자가 지킨다(넘으면 assert).
    PlanarView Slice(uint32 frameOffset, uint32 frames) const noexcept;
};

// 읽기 전용 planar 보기다. PlanarView에서 암시적으로 만들 수 있다.
struct PlanarConstView {
    const float* channels[MaxOutputChannels] = {};
    uint32 channelCount = 0;
    uint32 frameCount = 0;

    PlanarConstView() noexcept = default;

    PlanarConstView(const PlanarView& view) noexcept;

    const float* Channel(uint32 index) const noexcept {
        return channels[index];
    }

    PlanarConstView Slice(uint32 frameOffset, uint32 frames) const noexcept;
};

// 채널 수 × frame 용량의 float32를 한 번에 할당해 소유하는 버퍼다. 채널마다 시작 주소가
// AudioBufferAlignment에 맞는다. 처음 내용은 0이다. 이동만 된다.
class PlanarBuffer {
public:
    // 버퍼를 만든다.
    // - channels: 1–MaxOutputChannels, frameCapacity: 1–MaxBlockFrames. 밖이면 InvalidArgument.
    // - 할당 실패는 OutOfMemory다. allocator는 버퍼보다 오래 살아야 한다.
    [[nodiscard]] static Result<PlanarBuffer> TryCreate(uint32 channels, uint32 frameCapacity,
                                                        Allocator& allocator) noexcept;

    PlanarBuffer() noexcept = default;
    PlanarBuffer(PlanarBuffer&& other) noexcept;
    PlanarBuffer& operator=(PlanarBuffer&& other) noexcept;
    PlanarBuffer(const PlanarBuffer&) = delete;
    PlanarBuffer& operator=(const PlanarBuffer&) = delete;
    ~PlanarBuffer() noexcept;

    uint32 ChannelCount() const noexcept {
        return channelCount_;
    }

    uint32 FrameCapacity() const noexcept {
        return frameCapacity_;
    }

    float* Channel(uint32 index) const noexcept;

    // 앞 frames frame의 보기다. frames는 FrameCapacity() 이하여야 한다(넘으면 assert).
    PlanarView View(uint32 frames) const noexcept;

    PlanarView View() const noexcept {
        return View(frameCapacity_);
    }

    // 모든 채널의 용량 전체를 0으로 채운다.
    void Clear() noexcept;

private:
    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    float* data_ = nullptr;
    uint32 channelCount_ = 0;
    uint32 frameCapacity_ = 0;
    // 채널 사이 간격(float 수)이다. frameCapacity_를 정렬 단위로 올린 값이다.
    uint32 channelStride_ = 0;
};

// 모든 채널을 0으로 채운다.
void Clear(const PlanarView& destination) noexcept;

// destination += source × gain. 두 보기의 채널 수·frame 수가 같아야 한다(다르면 assert).
void MixAdd(const PlanarView& destination, const PlanarConstView& source, float gain) noexcept;

// destination += source × g(n). n번째 frame(0부터)의 이득은 start + (end − start)·n/N이다(N = frame
// 수, UE Audio::ArrayMixIn의 시작·끝 이득 규칙). 마지막 frame은 end에 닿지 않고 다음 블록이 end에서
// 시작한다. 두 보기의 채널 수·frame 수가 같아야 한다.
void MixAddRamp(const PlanarView& destination, const PlanarConstView& source, float startGain,
                float endGain) noexcept;

// 버퍼 전체에 gain을 곱한다.
void Scale(const PlanarView& destination, float gain) noexcept;

} // namespace rocks::audio
