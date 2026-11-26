#pragma once

#include <Base/Types.h>

// 원본 디코더 fixture의 기준값이다(Docs/Design/AudioSystem 3.26–3.29·3.36.5절). 디코더 시험과
// 임포터 시험이 같은 값을 단언한다 — 임포터 bulk(Float32 planar little-endian)가 디코더 결과 byte와
// 같으므로 "디코드 SHA 고정값" 완료 기준을 두 시험이 함께 묶는다. SHA는 x86-64 Windows(UCRT
// libm·SSE2) 값이다(ARM64·다른 OS 고정값은 그 기기 측정 뒤 — 3.26·3.29절).
namespace rocks::audio::sourcereference {

// FLAC fixture 길이다.
inline constexpr uint64 FlacStereo16Frames = 1000;
inline constexpr uint64 FlacMono24Frames = 600;

// Ogg Vorbis stereo(마지막 page granule)와 그 디코드 결과 SHA-256이다.
inline constexpr uint64 VorbisStereoFrames = 11025;
inline constexpr const char* VorbisStereoDigestX64 =
    "df80529fcc7983afcd3810f287a338aad25381eeb623e73ade462c6909028a84";

// Ogg Opus stereo(pre-skip 제거 뒤)와 그 디코드 결과 SHA-256이다.
inline constexpr uint64 OpusStereoFrames = 12000;
inline constexpr const char* OpusStereoDigestX64 =
    "d7786b91f08292f101f372a4a37449cfe967c3fcfe117fadbb6dedf02695d518";

// MP3 mono(LAME tag delay·padding 제거 뒤)와 그 디코드 결과 SHA-256이다.
inline constexpr uint64 Mp3MonoLameFrames = 11025;
inline constexpr const char* Mp3MonoLameDigestX64 =
    "98c3cb062812ef8c6b6995fc3343f62df2b2c5f7b3a483d4b721c38996a01b4c";

} // namespace rocks::audio::sourcereference
