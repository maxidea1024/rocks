#pragma once

#include <AudioTools/AudioEncoder.h>

// 쿠킹용 Opus 인코더다(Docs/Design/AudioSystem.ko.md 3.32.3절). UE AudioFormatOpus와 같이 20 ms
// 패킷, 품질 1–100 → 채널당 16–96 kbps를 쓰고, libopus multistream surround 인코더(mapping
// family 1)로 채널 1–8을 한 경로로 인코드한다. 입력은 48 kHz만 받는다(다른 rate는
// ResampleForCooking으로 먼저 바꾼다).
//
// 사용 예:
//   OpusAudioEncoder encoder(allocator);
//   EncodedAudio encoded(allocator);
//   AudioEncodeSettings settings;
//   settings.quality = 60;
//   if (auto result = encoder.Encode(pcm, 48000, DefaultAudioChannelMask(2), settings, encoded);
//       !result) { ... }
namespace rocks::audio {

// 품질별 비트레이트(bps)다: (16000 + (quality − 1) × 80000 / 99, 내림) × channels. UE
// GetBitRateFromQuality와 같은 값이다. quality가 1–100 밖이거나 channels가 1–8 밖이면 0이다.
ROCKS_AUDIOTOOLS_API uint32 OpusBitrateForQuality(uint32 quality, uint32 channels) noexcept;

class ROCKS_AUDIOTOOLS_API OpusAudioEncoder final : public AudioEncoder {
public:
    // allocator는 인코드 중 libopus 인코더 상태와 scratch를 잡는다. 인코더보다 오래 살아야 한다.
    explicit OpusAudioEncoder(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    const AudioCodecInfo& Info() const noexcept override;

    // AudioEncoder::Encode 오류에 더해: rate ≠ 48000·품질 1–100 밖은 InvalidArgument, 채널
    // 마스크가 그 채널 수의 기본 배치(DefaultAudioChannelMask)가 아니면 Unsupported, libopus 오류는
    // Unsupported다. 결과 단위는 [u16 LE 길이][multistream 패킷](960 codec frame)이고 encoder
    // delay = libopus lookahead, preRollFrames = OpusPreRollFrames(15360 = 320 ms)이다.
    Result<void> Encode(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                        const AudioEncodeSettings& settings, EncodedAudio& out) noexcept override;

private:
    Allocator& allocator_;
};

} // namespace rocks::audio
