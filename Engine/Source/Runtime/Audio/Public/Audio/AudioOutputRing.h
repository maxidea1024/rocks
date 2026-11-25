#pragma once

#include <Audio/AudioOutput.h>
#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Sync/RealtimeWakeSignal.h>

namespace rocks::audio {

// 출력 링 칸 수의 하한·상한이다(Docs/Design/AudioSystem 3.2절, UE NumOutputBuffers 하한 2와 같다).
inline constexpr uint32 MinOutputBufferCount = 2;
inline constexpr uint32 MaxOutputBufferCount = 16;

// 출력 링 칸 수다: max(MinOutputBufferCount, ceil(callbackFrames / blockFrames) + 1, requested)를
// MaxOutputBufferCount로 자른다. 콜백 한 번이 callbackFrames를 가져가는 동안 AudioRT가 블록을 하나
// 더 채울 수 있어야 구조적 underrun이 없다(3.2절). blockFrames가 0이면 MinOutputBufferCount다.
//
// 사용 예: ChooseOutputBufferCount(480, 256, 0) == 3
ROCKS_AUDIO_API uint32 ChooseOutputBufferCount(uint32 callbackFrames, uint32 blockFrames,
                                               uint32 requested) noexcept;

// 출력 링 계수 스냅샷이다(어느 스레드에서나 읽는다).
struct AudioOutputRingCounters {
    // 링이 비어 무음으로 채운 frame 수와 그런 콜백 수다.
    uint64 underrunFrames = 0;
    uint64 underrunCallbacks = 0;
    // 장치 콜백이 가져간 frame 수 누계다.
    uint64 consumedFrames = 0;
};

// AudioRT(생산자 하나)와 장치 콜백(소비자 하나) 사이의 블록 링이다(3.33.2절). 칸은 Q frame × 채널
// float이다. 생산자는 블록을 통째로 쓰고, 소비자는 아무 frame 수나 복사하며 블록 안 읽기 위치를
// 자기 상태로 둔다. 잠금·할당·대기가 없다.
//
// 메모리 순서: 생산자는 칸을 쓴 뒤 written을 release로 올리고 소비자는 acquire로 읽은 뒤 칸을
// 읽는다. 소비자는 칸을 다 읽은 뒤 read를 release로 올리고 생산자는 acquire로 본 뒤 칸을 다시 쓴다.
//
// 스레드 계약: BeginWrite·CommitWrite·HasFreeSlot은 생산자, Read는 소비자 한 스레드만
// 부른다(스레드가 바뀌면 그 사이에 happens-before가 있어야 한다). Initialize·Reset·Shutdown은 둘 다
// 멈춘 뒤 한다.
//
// 사용 예:
//   AudioOutputRing ring;
//   (void)ring.Initialize(allocator, bufferCount, blockFrames, channels, &wake);
//   // AudioRT: while (ring.HasFreeSlot()) { Render(ring.BeginWrite()); ring.CommitWrite(); }
//   // 장치 콜백: ring.Read(view);
class ROCKS_AUDIO_API AudioOutputRing {
public:
    AudioOutputRing() noexcept = default;
    AudioOutputRing(const AudioOutputRing&) = delete;
    AudioOutputRing& operator=(const AudioOutputRing&) = delete;

    ~AudioOutputRing() noexcept {
        Shutdown();
    }

    // 칸을 할당한다. wake는 소비자가 칸을 비웠을 때 깨울 신호다(nullptr이면 깨우지 않는다). 실패:
    // bufferCount가 [MinOutputBufferCount, MaxOutputBufferCount] 밖·blockFrames 0·채널 0·이미
    // 초기화됨은 InvalidArgument, 크기 넘침은 Overflow, 그 밖은 allocator 오류(상태는 그대로).
    Result<void> Initialize(Allocator& allocator, uint32 bufferCount, uint32 blockFrames,
                            uint32 channels, platform::RealtimeWakeSignal* wake) noexcept;

    void Shutdown() noexcept;

    uint32 BufferCount() const noexcept {
        return bufferCount_;
    }

    uint32 BlockFrames() const noexcept {
        return blockFrames_;
    }

    uint32 Channels() const noexcept {
        return channels_;
    }

    // 생산자: 쓸 칸이 있는지다.
    bool HasFreeSlot() const noexcept;

    // 생산자: 다음에 쓸 칸이다(HasFreeSlot일 때만). blockFrames × channels float다.
    Span<float> BeginWrite() noexcept;

    // 생산자: BeginWrite로 쓴 칸을 공개한다.
    void CommitWrite() noexcept;

    // 소비자: view를 채운다. 칸이 모자라면 나머지를 무음으로 채우고 underrun을 센다. 블록을 하나
    // 이상 비웠으면 끝에서 깨우기 신호를 한 번 보낸다. view.channels는 링 채널 수와 같아야
    // 한다(다르면 전부 무음·underrun). Int16은 [-1, 1]로 자른 뒤 32767을 곱해 반올림한다.
    void Read(const AudioOutputView& view) noexcept;

    AudioOutputRingCounters CountersSnapshot() const noexcept;

    // 공개된 블록 누계·다 읽은 블록 누계다(진단·시험).
    uint64 WrittenBlocks() const noexcept {
        return written_.Load<platform::MemoryOrder::Acquire>();
    }

    uint64 ReadBlocks() const noexcept {
        return read_.Load<platform::MemoryOrder::Acquire>();
    }

private:
    void WriteSilence(const AudioOutputView& view, uint32 firstFrame) noexcept;

    float* samples_ = nullptr;
    Allocator* allocator_ = nullptr;
    platform::RealtimeWakeSignal* wake_ = nullptr;
    uint32 bufferCount_ = 0;
    uint32 blockFrames_ = 0;
    uint32 channels_ = 0;
    // 소비자 지역: 지금 읽는 칸 안의 frame 위치다.
    uint32 readOffset_ = 0;
    alignas(CacheLineSize) platform::Atomic<uint64> written_;
    alignas(CacheLineSize) platform::Atomic<uint64> read_;
    platform::Atomic<uint64> underrunFrames_;
    platform::Atomic<uint64> underrunCallbacks_;
    platform::Atomic<uint64> consumedFrames_;
};

} // namespace rocks::audio
