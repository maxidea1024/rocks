#include <AudioTools/AdpcmEncoder.h>

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::audio {

using platform::PlatformMath;

namespace {

// 예측식의 고정소수 배율(계수는 8bit 소수)과 int16 범위, nibble 범위다.
constexpr int32 CoefficientScale = 256;
constexpr int32 SampleMinimum = -32768;
constexpr int32 SampleMaximum = 32767;
constexpr int32 NibbleMinimum = -8;
constexpr int32 NibbleMaximum = 7;
constexpr uint32 NibbleMask = 0x0F;
constexpr uint32 NibbleBits = 4;

// float → int16 배율과 반올림 기준이다.
constexpr double QuantizeScale = 32768.0;
constexpr double HalfStep = 0.5;

// int16을 두 byte(LE)로 쓴다.
constexpr uint32 ByteBits = 8;
constexpr uint32 ByteMask = 0xFF;

int32 ClampInt(int32 value, int32 minimum, int32 maximum) noexcept {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

struct EncodeState {
    int32 coefficient1 = 0;
    int32 coefficient2 = 0;
    int32 delta = 0;
    int32 sample1 = 0;
    int32 sample2 = 0;
};

// UE EncodeNibble과 같은 식이다. 복원 표본을 reconstructed로 돌려준다.
uint32 EncodeNibble(EncodeState& state, int32 next, int32& reconstructed) noexcept {
    int32 predicted = (state.sample1 * state.coefficient1 + state.sample2 * state.coefficient2) /
                      CoefficientScale;
    const int32 errorDelta =
        ClampInt((next - predicted) / state.delta, NibbleMinimum, NibbleMaximum);
    predicted = ClampInt(predicted + state.delta * errorDelta, SampleMinimum, SampleMaximum);
    const uint32 nibble = static_cast<uint32>(errorDelta) & NibbleMask;
    state.sample2 = state.sample1;
    state.sample1 = predicted;
    const int32 adapted = (state.delta * AdpcmAdaptationTable[nibble]) / CoefficientScale;
    state.delta = adapted > AdpcmMinimumDelta ? adapted : AdpcmMinimumDelta;
    reconstructed = predicted;
    return nibble;
}

void WriteInt16(uint8* bytes, int32 value) noexcept {
    const uint16 bits = static_cast<uint16>(static_cast<int16>(value));
    bytes[0] = static_cast<uint8>(bits & ByteMask);
    bytes[1] = static_cast<uint8>((bits >> ByteBits) & ByteMask);
}

// 계수 색인 coefficient로 블록 하나를 block에 쓰고 제곱 오차 합을 돌려준다. samples는
// AdpcmFramesPerBlock개 (클립 끝 뒤는 0 — UE ReadFromArray와 같다).
uint64 EncodeBlock(const int16* samples, uint32 validFrames, uint32 coefficient,
                   uint8* block) noexcept {
    EncodeState state;
    state.coefficient1 = AdpcmCoefficient1[coefficient];
    state.coefficient2 = AdpcmCoefficient2[coefficient];
    state.delta = AdpcmAdaptationTable[0];
    // 첫 표본은 sample2, 두 번째는 sample1이다(디코더가 순서를 되돌린다).
    state.sample2 = samples[0];
    state.sample1 = samples[1];
    block[0] = static_cast<uint8>(coefficient);
    WriteInt16(block + 1, state.delta);
    WriteInt16(block + 3, state.sample1);
    WriteInt16(block + 5, state.sample2);

    uint64 error = 0;
    for (uint32 index = 0; index < AdpcmFramesPerBlock - 2; index += 2) {
        int32 first = 0;
        int32 second = 0;
        const uint32 high = EncodeNibble(state, samples[2 + index], first);
        const uint32 low = EncodeNibble(state, samples[3 + index], second);
        block[AdpcmPreambleBytes + index / 2] = static_cast<uint8>((high << NibbleBits) | low);
        // 마지막 블록의 0 채움 구간은 버려지므로 유효 frame의 오차만 더한다.
        const int64 firstError =
            2 + index < validFrames ? static_cast<int64>(samples[2 + index]) - first : 0;
        const int64 secondError =
            3 + index < validFrames ? static_cast<int64>(samples[3 + index]) - second : 0;
        error += static_cast<uint64>(firstError * firstError) +
                 static_cast<uint64>(secondError * secondError);
    }

    return error;
}

} // namespace

int16 QuantizePcm16(float sample) noexcept {
    // 비유한 값은 0이다(EncodeAdpcm은 그 전에 거부한다).
    if (!math::IsFinite(sample)) {
        return 0;
    }

    const double scaled = static_cast<double>(sample) * QuantizeScale;
    double rounded = PlatformMath::Floor(scaled + HalfStep);
    // 정확히 반이면 짝수 쪽으로(round-half-even).
    if (rounded - scaled == HalfStep && PlatformMath::Fmod(rounded, 2.0) != 0.0) {
        rounded -= 1.0;
    }

    const double clamped = math::Clamp(rounded, static_cast<double>(SampleMinimum),
                                       static_cast<double>(SampleMaximum));
    return static_cast<int16>(clamped);
}

Result<void> EncodeAdpcm(const PlanarConstView& input, Array<uint8>& out) noexcept {
    out.Clear();
    const uint32 channels = input.channelCount;
    const uint64 frames = input.frameCount;
    if (channels == 0 || channels > MaxOutputChannels || frames == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 channel = 0; channel < channels; ++channel) {
        for (uint64 frame = 0; frame < frames; ++frame) {
            if (!math::IsFinite(input.channels[channel][frame])) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    if (auto resized = out.TryResize(static_cast<usize>(AdpcmPayloadBytes(channels, frames)));
        !resized) {
        out.Clear();
        return resized;
    }

    const uint64 blocks = AdpcmBlockCount(frames);
    int16 samples[AdpcmFramesPerBlock];
    uint8 candidate[AdpcmBlockBytes];
    for (uint64 block = 0; block < blocks; ++block) {
        for (uint32 channel = 0; channel < channels; ++channel) {
            const uint64 first = block * AdpcmFramesPerBlock;
            const uint32 validFrames = frames - first < AdpcmFramesPerBlock
                                           ? static_cast<uint32>(frames - first)
                                           : AdpcmFramesPerBlock;
            for (uint32 index = 0; index < AdpcmFramesPerBlock; ++index) {
                const uint64 frame = first + index;
                samples[index] =
                    frame < frames ? QuantizePcm16(input.channels[channel][frame]) : int16{0};
            }

            // 계수 세트 7개 중 제곱 오차가 가장 작은 것(동률이면 낮은 색인)을 고른다.
            uint8* target = out.Data() + (block * channels + channel) * AdpcmBlockBytes;
            uint64 best = NumericLimits<uint64>::Max();
            for (uint32 coefficient = 0; coefficient < AdpcmCoefficientSets; ++coefficient) {
                const uint64 error = EncodeBlock(samples, validFrames, coefficient, candidate);
                if (error < best) {
                    best = error;
                    for (uint32 byte = 0; byte < AdpcmBlockBytes; ++byte) {
                        target[byte] = candidate[byte];
                    }
                }
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::audio
