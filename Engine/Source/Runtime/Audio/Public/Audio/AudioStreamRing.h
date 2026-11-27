#pragma once

#include <Audio/AudioBuffer.h>
#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>

// 스트림 PCM 링이다(Docs/Design/AudioSystem 3.40.1절). 디코드 서비스(생산자 하나)가 재생 순서대로
// planar float를 쓰고 AudioRT(소비자 하나)가 앞에서부터 읽는다. 쓰기·읽기 위치는 단조 증가하는
// 64비트 원자값이고, 스트림 상태(Running·Ended·Failed)는 쓰기 위치 word의 위 2 bit에 함께 넣는다.
// 그래서 생산자의 Release 저장 하나로 PCM·위치·상태가 같이 보인다.
//
// 사용 예(생산자):
//   if (ring.WritableFrames() >= unit.frameCount) { (void)ring.Write(unit); }
//   ring.MarkEnded();
// 사용 예(소비자 — AudioRT, 할당·대기 없음):
//   const uint32 readable = ring.ReadableFrames();
//   ring.Peek(frames, scratch);
//   ring.Consume(consumed);
//
// 스레드: 생산자 API는 생산자 스레드 하나, 소비자 API는 소비자 스레드 하나만 부른다. Reset은 둘 다
// 링을 쓰지 않을 때(슬롯 회수 뒤) 제어 쪽만 부른다.
namespace rocks::audio {

// 링 용량의 하한이다(3.40.1절): 믹서 조각 하나가 요구하는 입력이 MaxBlockFrames 이하이므로, 그 두
// 배 아래로 두면 굶음이 풀리지 않을 수 있다.
inline constexpr uint32 StreamRingMinFrames = 2 * MaxBlockFrames;
// 링 용량의 상한이다(위치 word의 62비트와 무관한 메모리 상한 — 48 kHz 약 350초).
inline constexpr uint32 StreamRingMaxFrames = 1u << 24;

// 스트림 상태다. Ended는 non-loop의 마지막 유효 frame까지 썼다는 뜻이다(그 뒤 쓰기 위치 =
// frameCount − startFrame). Failed는 디코드·데이터 오류다. 둘 다 끝 상태이고 바뀌지 않는다.
enum class AudioStreamState : uint8 {
    Running = 0,
    Ended = 1,
    Failed = 2
};

class ROCKS_AUDIO_API AudioStreamRing {
public:
    // 링을 만든다. channels는 담을 수 있는 최대 채널 수(1–MaxClipChannels(2))이고 처음 쓰는 채널
    // 수도 그 값이다(Reset으로 줄일 수 있다). 용량 [StreamRingMinFrames, StreamRingMaxFrames] 밖은
    // InvalidArgument, 할당 실패는 OutOfMemory다. allocator는 링보다 오래 살아야 한다.
    [[nodiscard]] static Result<UniquePtr<AudioStreamRing>>
    TryCreate(Allocator& allocator, uint32 channels, uint32 capacityFrames) noexcept;

    AudioStreamRing(Allocator& allocator, uint32 channels, uint32 capacityFrames) noexcept;
    AudioStreamRing(const AudioStreamRing&) = delete;
    AudioStreamRing& operator=(const AudioStreamRing&) = delete;

    uint32 ChannelCount() const noexcept {
        return channels_;
    }

    uint32 CapacityFrames() const noexcept {
        return capacity_;
    }

    // --- 생산자 ---

    // 지금 쓸 수 있는 frame 수다. 끝 상태면 0이다.
    uint32 WritableFrames() const noexcept;

    // pcm(채널 수 = ChannelCount, frame ≤ WritableFrames)을 뒤에 쓴다. 쓰기 전에 모든 표본이
    // 유한한지 본다 — 비유한이면 쓰지 않고 Failed로 바꾸고 CorruptData다(믹서 RegisterClip이 NaN을
    // 거르는 것과 같은 이유 — 필터 상태가 영구 NaN이 된다). 채널 수가 다르거나 frame이 넘치거나 끝
    // 상태면 InvalidArgument다.
    Result<void> Write(const PlanarConstView& pcm) noexcept;

    // 끝 상태로 바꾼다(이미 끝 상태면 아무것도 하지 않는다). 앞서 쓴 PCM보다 늦게 출판된다.
    void MarkEnded() noexcept;
    void MarkFailed() noexcept;

    // --- 소비자 ---

    // 읽을 수 있는 frame 수다.
    uint32 ReadableFrames() const noexcept;

    // 상태다. Ended·Failed를 보았으면 그때까지 쓴 PCM은 ReadableFrames에 이미 들어 있다.
    AudioStreamState State() const noexcept;

    // 앞 frames frame을 out(채널 수 = ChannelCount, frameCount ≥ frames)에 복사한다(소비하지 않음).
    // frames > ReadableFrames면 assert다.
    void Peek(uint32 frames, const PlanarView& out) const noexcept;

    // 앞 frames frame을 버린다. frames > ReadableFrames면 assert다.
    void Consume(uint32 frames) noexcept;

    // --- 제어(생산자·소비자가 없을 때) ---

    // 위치·상태를 처음으로 되돌리고 쓰는 채널 수를 channels(1–최대 채널 수)로 둔다(슬롯 재사용 —
    // 3.40.1절). 범위 밖이면 assert다.
    void Reset(uint32 channels) noexcept;

    // 만들 때 정한 최대 채널 수다.
    uint32 MaxChannelCount() const noexcept {
        return maxChannels_;
    }

private:
    // 쓰기 위치(아래 62 bit)와 상태(위 2 bit)다.
    struct alignas(CacheLineSize) WriteSide {
        uint64 word = 0;
    };

    struct alignas(CacheLineSize) ReadSide {
        uint64 position = 0;
    };

    WriteSide write_;
    ReadSide read_;
    Array<float> samples_;
    uint32 channels_ = 0;
    uint32 maxChannels_ = 0;
    uint32 capacity_ = 0;
};

} // namespace rocks::audio
