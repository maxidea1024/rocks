#pragma once

#include <Audio/AudioBuffer.h>
#include <Core/Containers/Span.h>

// MS ADPCM 블록 코덱의 형식 상수와 디코더다(Docs/Design/AudioSystem.ko.md 3.24절). UE
// AdpcmAudioDecoder (Runtime/AdpcmAudioDecoder/Module/Private/Adpcm.cpp)와 같은 형식·식이다.
// 인코더는 AudioTools에 있다.
//
// 블록 하나는 채널 하나의 1012 frame이다: 머리 7 byte(계수 색인, 초기 delta, 두 번째·첫 표본 int16
// LE)와 nibble 쌍(상위 nibble 먼저). 여러 채널은 블록 묶음 k = [채널 0 블록 k][채널 1 블록 k]…로
// 교차한다.
//
// 사용 예:
//   // payload: 채널 2, 3000 frame 클립의 ADPCM 데이터. 1500 frame부터 256 frame을 디코드한다.
//   auto decoded = DecodeAdpcmFrames(payload, 2, 3000, 1500, output.View(256));
namespace rocks::audio {

// 블록 크기(byte)와 머리 크기, 블록당 frame 수다(UE AudioFormatADPCM 값).
inline constexpr uint32 AdpcmBlockBytes = 512;
inline constexpr uint32 AdpcmPreambleBytes = 7;
inline constexpr uint32 AdpcmFramesPerBlock = (AdpcmBlockBytes - AdpcmPreambleBytes) * 2 + 2;

// 계수 세트 수, 적응표 크기, delta 하한이다(MS ADPCM 표준).
inline constexpr uint32 AdpcmCoefficientSets = 7;
inline constexpr uint32 AdpcmAdaptationEntries = 16;
inline constexpr int32 AdpcmMinimumDelta = 16;

// 디코더가 손상 데이터의 정수 넘침을 막는 delta 상한이다. 유효 스트림의 delta는 표본 범위(2^16)
// 근처에서 멈추므로 이 상한은 정상 결과를 바꾸지 않는다.
inline constexpr int32 AdpcmMaximumDelta = 1 << 20;

// 예측 계수·적응표다(UE GetAdaptationCoefficients·GetAdaptationTable).
inline constexpr int32 AdpcmCoefficient1[AdpcmCoefficientSets] = {256, 512, 0, 192, 240, 460, 392};
inline constexpr int32 AdpcmCoefficient2[AdpcmCoefficientSets] = {0, -256, 0, 64, 0, -208, -232};
inline constexpr int32 AdpcmAdaptationTable[AdpcmAdaptationEntries] = {
    230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

// int16 표본을 float로 바꾸는 배율(1/32768)이다.
inline constexpr float AdpcmSampleScale = 1.0f / 32768.0f;

// frames frame의 블록 묶음 수다(채널당 블록 수).
constexpr uint64 AdpcmBlockCount(uint64 frames) noexcept {
    return (frames + AdpcmFramesPerBlock - 1) / AdpcmFramesPerBlock;
}

// channels 채널·frames frame 클립의 payload 크기(byte)다.
constexpr uint64 AdpcmPayloadBytes(uint32 channels, uint64 frames) noexcept {
    return AdpcmBlockCount(frames) * channels * AdpcmBlockBytes;
}

// 블록 하나를 frames(1–AdpcmFramesPerBlock)만큼 디코드해 out[0..frames)에 float로 쓴다. 할당하지
// 않는다. 계수 색인이 6을 넘거나, block이 머리보다 짧거나, frames에 필요한 nibble보다 짧거나, out이
// frames보다 작으면 InvalidArgument이고 out을 쓰지 않는다.
ROCKS_AUDIO_API Result<void> DecodeAdpcmBlock(Span<const uint8> block, uint32 frames,
                                              Span<float> out) noexcept;

// 블록 묶음 배치의 payload에서 [startFrame, startFrame + out.frameCount)를 디코드한다(임의 위치
// 시작). out의 채널 수는 channels여야 한다. payload 크기가 AdpcmPayloadBytes(channels,
// totalFrames)와 다르거나 범위가 넘거나 블록이 잘못되면 InvalidArgument다(out은 일부 쓰였을 수
// 있다). 할당하지 않는다.
ROCKS_AUDIO_API Result<void> DecodeAdpcmFrames(Span<const uint8> payload, uint32 channels,
                                               uint64 totalFrames, uint64 startFrame,
                                               const PlanarView& out) noexcept;

} // namespace rocks::audio
