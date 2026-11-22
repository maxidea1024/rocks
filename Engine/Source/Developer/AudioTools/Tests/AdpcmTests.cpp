#include <Audio/Adpcm.h>
#include <AudioTools/AdpcmEncoder.h>
#include <Base/InitializerList.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace adpcm_tests_file {

using namespace rocks::audio;
using platform::PlatformMath;

// ADPCM 블록 코덱 시험이다(Docs/Design/AudioSystem.ko.md 3.24.4절). 정답은 UE Adpcm.cpp·
// AudioFormatADPCM.cpp의 식을 이 파일에 따로 옮긴 참조 구현이고, 표(계수·적응)도 시험 대상 헤더를
// 쓰지 않고 리터럴로 둔다(표 값 오류를 잡기 위해서다).

namespace {

constexpr uint32 BlockFrames = 1012;
constexpr uint32 BlockBytes = 512;
constexpr uint32 PreambleBytes = 7;
constexpr int32 ReferenceC1[7] = {256, 512, 0, 192, 240, 460, 392};
constexpr int32 ReferenceC2[7] = {0, -256, 0, 64, 0, -208, -232};
constexpr int32 ReferenceAdaptation[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                                           768, 614, 512, 409, 307, 230, 230, 230};
constexpr int32 ReferenceMinimumDelta = 16;
constexpr int32 ReferenceInitialDelta = 230;
constexpr int32 SampleLow = NumericLimits<int16>::Min();
constexpr int32 SampleHigh = NumericLimits<int16>::Max();
constexpr float SampleScale = 32768.0f;

// 시험 반복 수와 품질 하한이다.
constexpr uint32 RandomBlockRounds = 200;
constexpr uint32 CoefficientRounds = 8;
constexpr double MinimumSineSnrDb = 20.0;

int32 ClampReference(int32 value) noexcept {
    return value < SampleLow ? SampleLow : (value > SampleHigh ? SampleHigh : value);
}

// UE DecodeBlock 참조 구현(int16 출력)이다.
void ReferenceDecode(const uint8* block, uint32 frames, int16* out) noexcept {
    const uint32 coefficient = block[0];
    int32 delta = static_cast<int16>(block[1] | (block[2] << 8));
    int32 sample1 = static_cast<int16>(block[3] | (block[4] << 8));
    int32 sample2 = static_cast<int16>(block[5] | (block[6] << 8));
    out[0] = static_cast<int16>(sample2);
    if (frames > 1) {
        out[1] = static_cast<int16>(sample1);
    }

    for (uint32 index = 2; index < frames; ++index) {
        const uint8 pair = block[PreambleBytes + (index - 2) / 2];
        const uint32 nibble = (index % 2 == 0) ? pair >> 4 : pair & 0x0F;
        const int32 signedNibble = static_cast<int32>(nibble ^ 8u) - 8;
        const int32 predicted = ClampReference(
            (sample1 * ReferenceC1[coefficient] + sample2 * ReferenceC2[coefficient]) / 256 +
            signedNibble * delta);
        sample2 = sample1;
        sample1 = predicted;
        delta = (delta * ReferenceAdaptation[nibble]) / 256;
        delta = delta < ReferenceMinimumDelta ? ReferenceMinimumDelta : delta;
        out[index] = static_cast<int16>(predicted);
    }
}

// UE EncodeBlock 참조 구현(계수 색인 지정)이다. block에 쓰고 유효 frame의 제곱 오차 합을 돌려준다.
uint64 ReferenceEncode(const int16* samples, uint32 validFrames, uint32 coefficient,
                       uint8* block) noexcept {
    int32 delta = ReferenceInitialDelta;
    int32 sample2 = samples[0];
    int32 sample1 = samples[1];
    block[0] = static_cast<uint8>(coefficient);
    block[1] = static_cast<uint8>(delta & 0xFF);
    block[2] = static_cast<uint8>((delta >> 8) & 0xFF);
    block[3] = static_cast<uint8>(sample1 & 0xFF);
    block[4] = static_cast<uint8>((sample1 >> 8) & 0xFF);
    block[5] = static_cast<uint8>(sample2 & 0xFF);
    block[6] = static_cast<uint8>((sample2 >> 8) & 0xFF);
    uint64 error = 0;
    for (uint32 index = 2; index < BlockFrames; ++index) {
        int32 predicted =
            (sample1 * ReferenceC1[coefficient] + sample2 * ReferenceC2[coefficient]) / 256;
        int32 errorDelta = (samples[index] - predicted) / delta;
        errorDelta = errorDelta < -8 ? -8 : (errorDelta > 7 ? 7 : errorDelta);
        predicted = ClampReference(predicted + delta * errorDelta);
        const uint32 nibble = static_cast<uint32>(errorDelta) & 0x0F;
        sample2 = sample1;
        sample1 = predicted;
        delta = (delta * ReferenceAdaptation[nibble]) / 256;
        delta = delta < ReferenceMinimumDelta ? ReferenceMinimumDelta : delta;
        uint8& pair = block[PreambleBytes + (index - 2) / 2];
        pair = index % 2 == 0 ? static_cast<uint8>(nibble << 4) : static_cast<uint8>(pair | nibble);
        if (index < validFrames) {
            const int64 difference = static_cast<int64>(samples[index]) - predicted;
            error += static_cast<uint64>(difference * difference);
        }
    }

    return error;
}

// 참조 인코더로 계수 7개 중 오차 최소(동률이면 낮은 색인) 블록을 만든다.
void ReferenceBestBlock(const int16* samples, uint32 validFrames, uint8* best) noexcept {
    uint8 candidate[BlockBytes];
    uint64 bestError = NumericLimits<uint64>::Max();
    for (uint32 coefficient = 0; coefficient < 7; ++coefficient) {
        const uint64 error = ReferenceEncode(samples, validFrames, coefficient, candidate);
        if (error < bestError) {
            bestError = error;
            for (uint32 byte = 0; byte < BlockBytes; ++byte) {
                best[byte] = candidate[byte];
            }
        }
    }
}

PlanarBuffer MakeBuffer(uint32 channels, uint32 frames, Allocator& allocator) noexcept {
    auto created = PlanarBuffer::TryCreate(channels, frames, allocator);
    if (!created) {
        platform::Fatal("ADPCM test buffer was rejected");
    }

    return Move(created.Value());
}

void FillSine(float* samples, uint32 frames, float frequency, float amplitude,
              float phase = 0.0f) noexcept {
    for (uint32 frame = 0; frame < frames; ++frame) {
        samples[frame] =
            amplitude * PlatformMath::Sin(static_cast<float>(frame) * frequency + phase);
    }
}

double SnrDb(const float* reference, const float* decoded, uint32 frames) noexcept {
    double signal = 0.0;
    double noise = 0.0;
    for (uint32 frame = 0; frame < frames; ++frame) {
        signal += static_cast<double>(reference[frame]) * reference[frame];
        const double difference = static_cast<double>(reference[frame]) - decoded[frame];
        noise += difference * difference;
    }

    return 10.0 * PlatformMath::Log(signal / noise) / PlatformMath::Log(10.0);
}

} // namespace

TEST(Audio_Adpcm_TablesAndLayoutMatchUe) {
    for (uint32 index = 0; index < 7; ++index) {
        CHECK(AdpcmCoefficient1[index] == ReferenceC1[index] &&
              AdpcmCoefficient2[index] == ReferenceC2[index]);
    }

    for (uint32 index = 0; index < 16; ++index) {
        CHECK(AdpcmAdaptationTable[index] == ReferenceAdaptation[index]);
    }

    CHECK(AdpcmBlockBytes == BlockBytes && AdpcmPreambleBytes == PreambleBytes &&
          AdpcmFramesPerBlock == BlockFrames && AdpcmMinimumDelta == ReferenceMinimumDelta);
}

TEST(Audio_Adpcm_DecodeMatchesReference) {
    RandomStream random(0xADC);
    uint8 block[AdpcmBlockBytes];
    float decoded[BlockFrames];
    int16 reference[BlockFrames];
    // 대조 블록은 유효 스트림이다(무작위 byte는 적응 배율 평균이 1보다 커 delta가 발산한다). 신호
    // 종류·진폭을 바꿔 가며 참조 인코더로 만들고, 계수 색인도 돌려 가며 고정한다. 작은 진폭은 delta
    // 하한(16) 경로를 탄다.
    int16 signal[BlockFrames];
    float smooth = 0.0f;
    for (uint32 round = 0; round < RandomBlockRounds; ++round) {
        const float amplitude = round % 3 == 0 ? 0.0005f : (round % 3 == 1 ? 0.2f : 0.95f);
        for (uint32 frame = 0; frame < BlockFrames; ++frame) {
            smooth = round % 2 == 0 ? 0.95f * smooth + 0.05f * random.NextFloatInRange(-1.0f, 1.0f)
                                    : random.NextFloatInRange(-1.0f, 1.0f);
            signal[frame] = AdpcmQuantize(amplitude * smooth);
        }

        (void)ReferenceEncode(signal, BlockFrames, round % AdpcmCoefficientSets, block);
        REQUIRE(DecodeAdpcmBlock(Span<const uint8>(block), BlockFrames, Span<float>(decoded)));
        ReferenceDecode(block, BlockFrames, reference);
        for (uint32 frame = 0; frame < BlockFrames; ++frame) {
            CHECK(decoded[frame] == static_cast<float>(reference[frame]) / SampleScale);
        }

        // 부분 frame은 전체 디코드의 앞부분과 같다.
        for (const uint32 frames : {1u, 2u, 3u, BlockFrames - 1}) {
            float partial[BlockFrames];
            REQUIRE(DecodeAdpcmBlock(Span<const uint8>(block), frames, Span<float>(partial)));
            for (uint32 frame = 0; frame < frames; ++frame) {
                CHECK(partial[frame] == decoded[frame]);
            }
        }
    }

    // 손상 데이터(최대 delta + nibble 8 연속)도 넘치지 않고 [−1, 1) 안이다.
    uint8 hostile[AdpcmBlockBytes];
    for (uint8& byte : hostile) {
        byte = 0x88;
    }

    hostile[0] = 1;
    hostile[1] = 0xFF;
    hostile[2] = 0x7F;
    REQUIRE(DecodeAdpcmBlock(Span<const uint8>(hostile), BlockFrames, Span<float>(decoded)));
    for (const float sample : decoded) {
        CHECK(sample >= -1.0f && sample < 1.0f);
    }
}

TEST(Audio_Adpcm_QuantizeRoundsHalfToEven) {
    CHECK(AdpcmQuantize(0.0f) == 0);
    CHECK(AdpcmQuantize(0.5f / SampleScale) == 0);
    CHECK(AdpcmQuantize(1.5f / SampleScale) == 2);
    CHECK(AdpcmQuantize(2.5f / SampleScale) == 2);
    CHECK(AdpcmQuantize(-1.5f / SampleScale) == -2);
    CHECK(AdpcmQuantize(1.0f) == SampleHigh && AdpcmQuantize(-1.0f) == SampleLow &&
          AdpcmQuantize(4.0f) == SampleHigh);
    CHECK(AdpcmQuantize(PlatformMath::QuietNaN<float>()) == 0);
}

TEST(Audio_Adpcm_EncoderMatchesReferenceBytes) {
    // 인코더 출력을 참조 인코더(리터럴 표, 계수 탐색, 유효 frame 오차)와 byte 단위로 대조한다. 머리
    // 순서·nibble 순서·표 값·마지막 블록 0 채움이 모두 여기서 확인된다.
    SystemAllocator allocator;
    constexpr uint32 Frames = 3 * BlockFrames + 77;
    PlanarBuffer input = MakeBuffer(2, Frames, allocator);
    FillSine(input.Channel(0), Frames, 0.05f, 0.5f, 0.3f);
    RandomStream random(0x51);
    float smooth = 0.0f;
    for (uint32 frame = 0; frame < Frames; ++frame) {
        smooth = 0.9f * smooth + 0.1f * random.NextFloatInRange(-0.9f, 0.9f);
        input.Channel(1)[frame] = smooth;
    }

    Array<uint8> payload(allocator);
    REQUIRE(EncodeAdpcm(input.View(), payload));
    REQUIRE(payload.Size() == AdpcmPayloadBytes(2, Frames));
    int16 samples[BlockFrames];
    uint8 expected[BlockBytes];
    for (uint32 block = 0; block < AdpcmBlockCount(Frames); ++block) {
        for (uint32 channel = 0; channel < 2; ++channel) {
            const uint32 first = block * BlockFrames;
            const uint32 valid = Frames - first < BlockFrames ? Frames - first : BlockFrames;
            for (uint32 index = 0; index < BlockFrames; ++index) {
                samples[index] =
                    index < valid ? AdpcmQuantize(input.Channel(channel)[first + index]) : int16{0};
            }

            ReferenceBestBlock(samples, valid, expected);
            const uint8* actual = payload.Data() + (block * 2 + channel) * BlockBytes;
            for (uint32 byte = 0; byte < BlockBytes; ++byte) {
                CHECK(actual[byte] == expected[byte]);
            }
        }
    }

    // 머리 byte: +3·+4 = 두 번째 표본, +5·+6 = 첫 표본(0이 아닌 입력으로 직접 확인).
    const int16 firstSample = AdpcmQuantize(input.Channel(0)[0]);
    const int16 secondSample = AdpcmQuantize(input.Channel(0)[1]);
    CHECK(firstSample != 0 && secondSample != 0);
    CHECK(static_cast<int16>(payload[3] | (payload[4] << 8)) == secondSample);
    CHECK(static_cast<int16>(payload[5] | (payload[6] << 8)) == firstSample);

    // 왕복 품질과 결정성.
    PlanarBuffer decoded = MakeBuffer(2, Frames, allocator);
    REQUIRE(DecodeAdpcmFrames(Span<const uint8>(payload.Data(), payload.Size()), 2, Frames, 0,
                              decoded.View()));
    CHECK(SnrDb(input.Channel(0), decoded.Channel(0), Frames) > MinimumSineSnrDb);
    Array<uint8> again(allocator);
    REQUIRE(EncodeAdpcm(input.View(), again));
    CHECK(again.Size() == payload.Size());
    for (usize index = 0; index < payload.Size(); ++index) {
        CHECK(again[index] == payload[index]);
    }

    // 계수 탐색은 UE(색인 0 고정)보다 오차가 크지 않다.
    for (uint32 round = 0; round < CoefficientRounds; ++round) {
        for (uint32 frame = 0; frame < BlockFrames; ++frame) {
            smooth = 0.9f * smooth + 0.1f * random.NextFloatInRange(-0.9f, 0.9f);
            samples[frame] =
                AdpcmQuantize(round % 2 == 0 ? smooth : random.NextFloatInRange(-0.3f, 0.3f));
        }

        uint8 scratch[BlockBytes];
        ReferenceBestBlock(samples, BlockFrames, expected);
        const uint64 best = ReferenceEncode(samples, BlockFrames, expected[0], scratch);
        CHECK(best <= ReferenceEncode(samples, BlockFrames, 0, scratch));
    }
}

TEST(Audio_Adpcm_RandomAccessMatchesFullDecode) {
    SystemAllocator allocator;
    constexpr uint32 Frames = 2 * BlockFrames + 500;
    // 서로 다른 0 아닌 스테레오로 채널 교차 오프셋(블록 1 이상, 블록 안 시작)을 확인한다.
    PlanarBuffer input = MakeBuffer(2, Frames, allocator);
    FillSine(input.Channel(0), Frames, 0.013f, 0.7f);
    FillSine(input.Channel(1), Frames, 0.031f, 0.4f, 1.1f);
    Array<uint8> payload(allocator);
    REQUIRE(EncodeAdpcm(input.View(), payload));
    PlanarBuffer full = MakeBuffer(2, Frames, allocator);
    const Span<const uint8> bytes(payload.Data(), payload.Size());
    REQUIRE(DecodeAdpcmFrames(bytes, 2, Frames, 0, full.View()));
    PlanarBuffer part = MakeBuffer(2, 1500, allocator);
    for (const uint32 start : {0u, 1u, BlockFrames - 1, BlockFrames, BlockFrames + 333,
                               2 * BlockFrames + 7, Frames - 1}) {
        const uint32 count = Frames - start < 1500 ? Frames - start : 1500;
        REQUIRE(DecodeAdpcmFrames(bytes, 2, Frames, start, part.View(count)));
        for (uint32 channel = 0; channel < 2; ++channel) {
            for (uint32 frame = 0; frame < count; ++frame) {
                CHECK(part.Channel(channel)[frame] == full.Channel(channel)[start + frame]);
            }
        }
    }

    // 크기 경계: 1 frame과 정확히 한 블록.
    for (const uint32 frames : {1u, BlockFrames, BlockFrames + 1}) {
        PlanarBuffer small = MakeBuffer(1, frames, allocator);
        FillSine(small.Channel(0), frames, 0.1f, 0.25f, 0.5f);
        Array<uint8> encoded(allocator);
        REQUIRE(EncodeAdpcm(small.View(), encoded));
        CHECK(encoded.Size() == AdpcmPayloadBytes(1, frames));
        PlanarBuffer back = MakeBuffer(1, frames, allocator);
        REQUIRE(DecodeAdpcmFrames(Span<const uint8>(encoded.Data(), encoded.Size()), 1, frames, 0,
                                  back.View()));
        CHECK(back.Channel(0)[0] ==
              static_cast<float>(AdpcmQuantize(small.Channel(0)[0])) / SampleScale);
    }
}

TEST(Audio_Adpcm_RejectsInvalidInput) {
    SystemAllocator allocator;
    uint8 block[AdpcmBlockBytes] = {};
    float out[BlockFrames];
    block[0] = static_cast<uint8>(AdpcmCoefficientSets);
    CHECK(DecodeAdpcmBlock(Span<const uint8>(block), 10, Span<float>(out)).Error() ==
          ErrorCode::InvalidArgument);
    block[0] = 0;
    CHECK(DecodeAdpcmBlock(Span<const uint8>(block, PreambleBytes - 1), 1, Span<float>(out))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmBlock(Span<const uint8>(block, PreambleBytes + 1), 5, Span<float>(out))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmBlock(Span<const uint8>(block), 0, Span<float>(out)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmBlock(Span<const uint8>(block), BlockFrames + 1, Span<float>(out)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmBlock(Span<const uint8>(block), 4, Span<float>(out, 3)).Error() ==
          ErrorCode::InvalidArgument);

    PlanarBuffer view = MakeBuffer(1, 16, allocator);
    // 크기가 맞는 payload(100 frame = 블록 하나)는 디코드된다.
    CHECK(DecodeAdpcmFrames(Span<const uint8>(block), 1, 100, 0, view.View()));
    CHECK(DecodeAdpcmFrames(Span<const uint8>(block, 100), 1, 100, 0, view.View()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmFrames(Span<const uint8>(block), 1, 100, 90, view.View()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmFrames(Span<const uint8>(block), 1, 100, 101, view.View(0)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmFrames(Span<const uint8>(block), 2, 100, 0, view.View()).Error() ==
          ErrorCode::InvalidArgument);
    // 손상된 frame 수(넘침 경계)는 Fatal 없이 거부한다.
    CHECK(
        DecodeAdpcmFrames(Span<const uint8>(block), 1, NumericLimits<uint64>::Max(), 0, view.View())
            .Error() == ErrorCode::InvalidArgument);
    CHECK(DecodeAdpcmFrames(Span<const uint8>(block), 1, BlockFrames + 1, 0, view.View()).Error() ==
          ErrorCode::InvalidArgument);

    for (const float poison : {PlatformMath::QuietNaN<float>(), PlatformMath::Infinity<float>()}) {
        PlanarBuffer poisoned = MakeBuffer(1, 4, allocator);
        poisoned.Channel(0)[2] = poison;
        Array<uint8> encoded(allocator);
        CHECK(EncodeAdpcm(poisoned.View(), encoded).Error() == ErrorCode::InvalidArgument);
        CHECK(encoded.Size() == 0);
    }
}

} // namespace adpcm_tests_file
