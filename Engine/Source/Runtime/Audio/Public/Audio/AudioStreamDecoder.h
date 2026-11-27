#pragma once

#include <Audio/AudioChunkSource.h>
#include <Audio/AudioDecoder.h>
#include <Audio/AudioStreamRing.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>

// 스트림 생산자다(Docs/Design/AudioSystem 3.40.2절). 플랫폼 데이터 하나를 디코더 하나로 풀어 재생
// 순서대로 스트림 링에 쓴다.
// - 시작: 유효 frame s의 codec frame(s + encoderDelay)을 담은 청크에 BeginChunk한다(Opus는
// pre-roll로
//   수렴한 근사, PCM16·ADPCM은 정확).
// - 순차: 청크 끝에서 ContinueChunk로 상태를 이어 연다(전체 디코드와 bit까지 같다).
// - loop: loop 끝까지 쓰고 남은 단위 PCM을 버린 뒤, 같은 디코더로 loop 시작에 BeginChunk한다
//   (BeginChunk는 결정적이라 이음매 뒤 PCM은 늘 같다 — UE PrepareToLoop).
// - 출력은 유효 frame 구간(앞 delay·끝 padding을 자른 구간)만이다. 디코더가 청크 frame 수와 다르게
//   내거나 디코드·청크 오류면 링을 Failed로, non-loop가 끝까지 쓰면 Ended로 둔다.
//
// 사용 예(제어 스레드가 만들고 디코드 스레드가 Pump한다):
//   auto decoder = table.Create(view.Header().codec, allocator);
//   AudioStreamDecoderDesc desc{&view, &chunks, startFrame, loop, loopBegin, loopEnd};
//   auto stream = AudioStreamDecoder::TryCreate(allocator, Move(decoder.Value()), desc);
//   stream.Value()->Pump(ring);   // 디코드 스레드
//
// 스레드: TryCreate는 아무 스레드(보통 제어 쪽), Pump는 한 스레드(디코드 서비스)만 부른다.
namespace rocks::audio {

struct AudioStreamDecoderDesc {
    // 머리·청크 표를 읽는 보기다(payload는 chunks가 준다). 보기와 chunks는 디코더보다 오래 살아야
    // 한다.
    const AudioPlatformDataView* view = nullptr;
    AudioChunkSource* chunks = nullptr;
    // 시작 유효 frame과 loop 구간(유효 frame — 둘 다 0이면 clip 전체)이다.
    uint32 startFrame = 0;
    bool loop = false;
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
};

class ROCKS_AUDIO_API AudioStreamDecoder {
public:
    // 디코더를 Open하고 단위 scratch를 잡는다. 오류: view·chunks가 null, 채널이 2보다 많음,
    // startFrame ≥ frames, loop 구간이 frames 밖·비었음, loop인데 startFrame ≥ loop 끝
    // InvalidArgument, 디코더 Open의 오류(Unsupported·CorruptData·InvalidArgument), 할당 실패
    // OutOfMemory.
    [[nodiscard]] static Result<UniquePtr<AudioStreamDecoder>>
    TryCreate(Allocator& allocator, UniquePtr<AudioDecoder> decoder,
              const AudioStreamDecoderDesc& desc) noexcept;

    AudioStreamDecoder(Allocator& allocator, UniquePtr<AudioDecoder> decoder,
                       const AudioStreamDecoderDesc& desc) noexcept;
    AudioStreamDecoder(const AudioStreamDecoder&) = delete;
    AudioStreamDecoder& operator=(const AudioStreamDecoder&) = delete;
    ~AudioStreamDecoder() noexcept;

    // ring의 빈 칸이 다 차거나, 청크가 Pending이거나, 끝 상태가 될 때까지 쓴다. ring의 쓰는 채널
    // 수는 플랫폼 데이터 채널 수와 같아야 한다(assert). 끝 상태(Ended·Failed)를 출판했거나 이미
    // 끝났으면 아무것도 하지 않는다. 할당하지 않는다.
    void Pump(AudioStreamRing& ring) noexcept;

    // 끝 상태를 출판했는지다(그 뒤 Pump는 아무것도 하지 않는다).
    // 열린 청크를 공급자에 돌려준다(멱등). 서비스가 취소·종료로 작업을 반납하기 전에 디코드 쪽에서
    // 부른다(공급자는 디코드 스레드 하나가 쓴다). 소멸자도 부르지만, 그때는 반납 뒤이거나 넘기기
    // 전이라 디코드 쪽이 쓰지 않는다.
    void ReleaseChunk() noexcept;

    bool IsFinished() const noexcept {
        return finished_;
    }

private:
    // 다음에 쓸 유효 frame(effective_)의 codec frame에서 디코드를 연다. 청크가 Pending이면 false다.
    bool OpenAtCursor(AudioStreamRing& ring) noexcept;
    // 지금 청크를 끝까지 냈다 — 다음 청크를 ContinueChunk로 연다. Pending이면 false다.
    bool ContinueNextChunk(AudioStreamRing& ring) noexcept;
    // 남은 단위 PCM(pending_)을 ring에 쓴다. 다 쓰면 true다.
    bool FlushPending(AudioStreamRing& ring) noexcept;
    void Fail(AudioStreamRing& ring) noexcept;
    // 지금 구간의 끝 유효 frame이다(loop면 loop 끝, 아니면 frames).
    uint64 SegmentEnd() const noexcept;

    UniquePtr<AudioDecoder> decoder_;
    AudioStreamDecoderDesc desc_;
    AudioPlatformDataHeader header_;
    Array<float> scratch_;
    PlanarView unit_{};
    // 다음에 링에 쓸 유효 frame이다(loop면 loop 구간 안).
    uint64 effective_ = 0;
    // 열린 청크와 그 안의 다음 codec frame이다.
    uint32 chunkIndex_ = 0;
    uint64 codecCursor_ = 0;
    bool chunkOpen_ = false;
    // 지금 청크를 다 냈고 다음 청크를 기다린다(Pending).
    bool awaitingNext_ = false;
    // 단위 scratch 안의 아직 쓰지 않은 유효 PCM이다.
    uint32 pendingOffset_ = 0;
    uint32 pendingFrames_ = 0;
    uint32 loopBegin_ = 0;
    uint32 loopEnd_ = 0;
    bool finished_ = false;
};

} // namespace rocks::audio
