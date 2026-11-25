#pragma once

#include <Base/Types.h>

// Vorbis 채널 순서(Opus mapping family 0·1도 같다)와 내부 순서(Docs/Design/AudioSystem 3.3절:
// FL, FR, FC, LFE, SL, SR, BL, BR, BC) 사이 변환이다. 원본 디코드(AudioTools, 3.28·3.29절), 쿠킹
// Opus 인코더(AudioTools)와 런타임 Opus 디코더(AudioOpus, 3.32절)가 같이 쓴다.
//
// 사용 예:
//   DeinterleaveVorbisOrder(decoded, channels, frames, planar);           // 디코드 뒤
//   InterleaveVorbisOrder(channelPointers, channels, first, count, packet); // 인코드 앞
namespace rocks::audio {

// VorbisChannelOrder[channels − 1][internal]은 내부 순서 채널 internal에 해당하는 Vorbis 순서
// 채널이다. 채널 수별 내부 배치: 1 M, 2 FL FR, 3 FL FR FC, 4 FL FR SL SR, 5 FL FR FC SL SR, 6 FL FR
// FC LFE SL SR, 7 FL FR FC LFE SL SR BC, 8 FL FR FC LFE SL SR BL BR. Vorbis의 rear 쌍은 4–6채널에서
// SL·SR, 8채널에서 BL·BR이다(3.28.2절).
inline constexpr uint32 MaxVorbisOrderChannels = 8;
inline constexpr uint8 VorbisChannelOrder[MaxVorbisOrderChannels][MaxVorbisOrderChannels] = {
    {0, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0}, {0, 2, 1, 0, 0, 0, 0, 0},
    {0, 1, 2, 3, 0, 0, 0, 0}, {0, 2, 1, 3, 4, 0, 0, 0}, {0, 2, 1, 5, 3, 4, 0, 0},
    {0, 2, 1, 6, 3, 4, 5, 0}, {0, 2, 1, 7, 3, 4, 5, 6}};

// Vorbis 순서 교차 표본(frames × channels)을 내부 순서 planar로 옮긴다. planar 채널 c는
// planar + c × planarStride부터 frames개다. channels는 1–8이다.
inline void DeinterleaveVorbisOrder(const float* interleaved, uint32 channels, uint64 frames,
                                    float* planar, uint64 planarStride) noexcept {
    const uint8* order = VorbisChannelOrder[channels - 1];
    for (uint64 index = 0; index < frames; ++index) {
        const float* frame = interleaved + static_cast<usize>(index) * channels;
        for (uint32 channel = 0; channel < channels; ++channel) {
            planar[static_cast<usize>(channel * planarStride + index)] = frame[order[channel]];
        }
    }
}

// planar 채널이 이어 붙은(채널마다 frames개) 경우다.
inline void DeinterleaveVorbisOrder(const float* interleaved, uint32 channels, uint64 frames,
                                    float* planar) noexcept {
    DeinterleaveVorbisOrder(interleaved, channels, frames, planar, frames);
}

// 내부 순서 planar 채널(channelData[c])의 [first, first + count) frame을 Vorbis 순서 교차로
// 옮긴다. channels는 1–8이다.
inline void InterleaveVorbisOrder(const float* const* channelData, uint32 channels, uint64 first,
                                  uint64 count, float* interleaved) noexcept {
    const uint8* order = VorbisChannelOrder[channels - 1];
    for (uint64 index = 0; index < count; ++index) {
        float* frame = interleaved + static_cast<usize>(index) * channels;
        for (uint32 channel = 0; channel < channels; ++channel) {
            frame[order[channel]] = channelData[channel][first + index];
        }
    }
}

} // namespace rocks::audio
