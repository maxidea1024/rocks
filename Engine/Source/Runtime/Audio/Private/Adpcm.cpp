#include <Audio/Adpcm.h>

namespace rocks::audio {

namespace {

// nibble 하나가 차지하는 bit 수와 nibble mask, 4bit 부호 확장 기준이다.
constexpr uint32 NibbleBits = 4;
constexpr uint32 NibbleMask = 0x0F;
constexpr int32 NibbleSignBit = 0x08;
constexpr int32 NibbleRange = 0x10;

// 예측식의 고정소수 배율(계수는 8bit 소수)과 int16 범위다.
constexpr int32 CoefficientScale = 256;
constexpr int32 SampleMinimum = -32768;
constexpr int32 SampleMaximum = 32767;

// 머리 안 필드 위치다(byte).
constexpr uint32 PreambleDeltaOffset = 1;
constexpr uint32 PreambleSecondSampleOffset = 3;
constexpr uint32 PreambleFirstSampleOffset = 5;

int16 ReadInt16(const uint8* bytes) noexcept {
    return static_cast<int16>(static_cast<uint16>(bytes[0]) | static_cast<uint16>(bytes[1] << 8));
}

int32 ClampSample(int64 value) noexcept {
    return value < SampleMinimum
               ? SampleMinimum
               : (value > SampleMaximum ? SampleMaximum : static_cast<int32>(value));
}

struct DecodeState {
    int32 coefficient1 = 0;
    int32 coefficient2 = 0;
    int32 delta = 0;
    int32 sample1 = 0;
    int32 sample2 = 0;
};

// UE DecodeNibble과 같은 식이다.
int32 DecodeNibble(DecodeState& state, uint32 nibble) noexcept {
    const int32 signedNibble = (static_cast<int32>(nibble) & NibbleSignBit) != 0
                                   ? static_cast<int32>(nibble) - NibbleRange
                                   : static_cast<int32>(nibble);
    // 손상 데이터에서 delta가 커져도 넘치지 않게 64비트로 계산하고 delta를 AdpcmMaximumDelta로
    // 자른다 (3.24.3절 — 유효 스트림의 delta는 이보다 훨씬 작아 결과가 UE와 같다).
    const int64 predicted = (static_cast<int64>(state.sample1) * state.coefficient1 +
                             static_cast<int64>(state.sample2) * state.coefficient2) /
                                CoefficientScale +
                            static_cast<int64>(signedNibble) * state.delta;
    const int32 sample = ClampSample(predicted);
    state.sample2 = state.sample1;
    state.sample1 = sample;
    const int64 adapted =
        static_cast<int64>(state.delta) * AdpcmAdaptationTable[nibble] / CoefficientScale;
    state.delta =
        adapted < AdpcmMinimumDelta
            ? AdpcmMinimumDelta
            : (adapted > AdpcmMaximumDelta ? AdpcmMaximumDelta : static_cast<int32>(adapted));
    return sample;
}

} // namespace

Result<void> DecodeAdpcmBlock(Span<const uint8> block, uint32 frames, Span<float> out) noexcept {
    if (frames == 0 || frames > AdpcmFramesPerBlock || out.Size() < frames ||
        block.Size() < AdpcmPreambleBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 머리 뒤 nibble 쌍 하나가 2 frame이다. 첫 두 frame은 머리에 있다.
    const uint32 nibbleFrames = frames > 2 ? frames - 2 : 0;
    if (block.Size() < AdpcmPreambleBytes + (nibbleFrames + 1) / 2) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8* bytes = block.Data();
    const uint32 coefficientIndex = bytes[0];
    if (coefficientIndex >= AdpcmCoefficientSets) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    DecodeState state;
    state.coefficient1 = AdpcmCoefficient1[coefficientIndex];
    state.coefficient2 = AdpcmCoefficient2[coefficientIndex];
    state.delta = ReadInt16(bytes + PreambleDeltaOffset);
    state.sample1 = ReadInt16(bytes + PreambleSecondSampleOffset);
    state.sample2 = ReadInt16(bytes + PreambleFirstSampleOffset);

    // 첫 두 표본은 머리에서 첫 표본·두 번째 표본 순서로 나온다(UE와 같다).
    out[0] = static_cast<float>(state.sample2) * AdpcmSampleScale;
    if (frames > 1) {
        out[1] = static_cast<float>(state.sample1) * AdpcmSampleScale;
    }

    for (uint32 index = 0; index < nibbleFrames; ++index) {
        const uint8 pair = bytes[AdpcmPreambleBytes + index / 2];
        const uint32 nibble =
            (index % 2 == 0) ? (pair >> NibbleBits) & NibbleMask : pair & NibbleMask;
        out[2 + index] = static_cast<float>(DecodeNibble(state, nibble)) * AdpcmSampleScale;
    }

    return Result<void>::Success();
}

Result<void> DecodeAdpcmFrames(Span<const uint8> payload, uint32 channels, uint64 totalFrames,
                               uint64 startFrame, const PlanarView& out) noexcept {
    if (channels == 0 || channels > MaxOutputChannels || out.channelCount != channels ||
        startFrame > totalFrames || out.frameCount > totalFrames - startFrame) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // payload 크기에서 블록 수를 역산해 넘침 없이 totalFrames와 맞춘다(손상된 frame 수가 와도 Fatal
    // 없이 거부).
    const uint64 groupBytes = static_cast<uint64>(channels) * AdpcmBlockBytes;
    const uint64 blocks = payload.Size() / groupBytes;
    if (payload.Size() % groupBytes != 0 || blocks == 0 ||
        totalFrames > blocks * AdpcmFramesPerBlock ||
        totalFrames <= (blocks - 1) * AdpcmFramesPerBlock) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 블록 하나 분량의 scratch다(스택, 4 KiB 남짓).
    float scratch[AdpcmFramesPerBlock];
    uint64 frame = startFrame;
    uint32 written = 0;
    while (written < out.frameCount) {
        const uint64 blockIndex = frame / AdpcmFramesPerBlock;
        const uint32 offsetInBlock = static_cast<uint32>(frame % AdpcmFramesPerBlock);
        const uint64 blockFirstFrame = blockIndex * AdpcmFramesPerBlock;
        const uint64 framesLeftInClip = totalFrames - blockFirstFrame;
        const uint32 blockFrames = framesLeftInClip < AdpcmFramesPerBlock
                                       ? static_cast<uint32>(framesLeftInClip)
                                       : AdpcmFramesPerBlock;
        const uint32 wanted = out.frameCount - written;
        const uint32 available = blockFrames - offsetInBlock;
        const uint32 take = wanted < available ? wanted : available;
        // 블록 앞부분부터 디코드해야 하므로 offset + take까지 디코드한다.
        const uint32 decodeFrames = offsetInBlock + take;
        for (uint32 channel = 0; channel < channels; ++channel) {
            const usize blockOffset =
                static_cast<usize>((blockIndex * channels + channel) * AdpcmBlockBytes);
            const Result<void> decoded =
                DecodeAdpcmBlock(payload.Slice(blockOffset, AdpcmBlockBytes), decodeFrames,
                                 Span<float>(scratch, AdpcmFramesPerBlock));
            if (!decoded) {
                return Result<void>::Failure(decoded.Error());
            }

            float* target = out.channels[channel] + written;
            for (uint32 index = 0; index < take; ++index) {
                target[index] = scratch[offsetInBlock + index];
            }
        }

        written += take;
        frame += take;
    }

    return Result<void>::Success();
}

} // namespace rocks::audio
