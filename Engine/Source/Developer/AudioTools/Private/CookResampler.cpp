#include <AudioTools/CookResampler.h>
#include <AudioTools/SourceAudio.h>

#include <Audio/AudioPlatformData.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

#include <samplerate.h>

namespace rocks::audio {

namespace {

// 위치·frame 변환이 받는 값의 상한(2^32)이다. × rate(2^18 미만)가 64비트를 넘지 않는다.
constexpr uint64 MaxConvertedPosition = uint64{1} << 32;

// libsamplerate 오류 번호 1은 할당 실패(SRC_ERR_MALLOC_FAILED)다.
constexpr int SampleRateMallocFailed = 1;

// libsamplerate에 한 번에 넣는 입력 frame 수다. 표본 수(frame × 채널)가 int·long(Windows 32비트)을
// 넘지 않는 작은 블록으로 나눠 넣고, 출력은 바로 planar에 옮긴다(원본 전체를 교차 사본으로 만들지
// 않는다).
constexpr uint32 InputBlockFrames = 4096;

// 한 번 처리에 받는 출력 여유 frame이다(블록 × 최대 비율 192000/8000 = 24에 더한다).
constexpr uint32 OutputBlockSlackFrames = 16;
constexpr uint32 MaxRateRatio = MaxPlatformSampleRate / MinPlatformSampleRate;

bool RateValid(uint32 rate) noexcept {
    return rate >= MinPlatformSampleRate && rate <= MaxPlatformSampleRate;
}

// value × target / source를 가까운 정수로(동률은 짝수) 바꾼다. 범위는 호출자가 확인했다.
uint64 RoundRatio(uint64 value, uint32 sourceRate, uint32 targetRate) noexcept {
    const uint64 numerator = value * targetRate;
    const uint64 quotient = numerator / sourceRate;
    const uint64 twiceRemainder = (numerator % sourceRate) * 2;
    if (twiceRemainder > sourceRate || (twiceRemainder == sourceRate && (quotient & 1) != 0)) {
        return quotient + 1;
    }

    return quotient;
}

// SRC_STATE 수명이다.
class SampleRateState {
public:
    SampleRateState(uint32 channels, int& error) noexcept
        : state_(src_new(SRC_SINC_BEST_QUALITY, static_cast<int>(channels), &error)) {
    }

    ~SampleRateState() {
        if (state_ != nullptr) {
            src_delete(state_);
        }
    }

    SampleRateState(const SampleRateState&) = delete;
    SampleRateState& operator=(const SampleRateState&) = delete;

    SRC_STATE* Get() const noexcept {
        return state_;
    }

private:
    SRC_STATE* state_;
};

ErrorCode SampleRateError(int error) noexcept {
    return error == SampleRateMallocFailed ? ErrorCode::OutOfMemory : ErrorCode::Unsupported;
}

// out의 기존 버퍼와 in이 겹치는지 본다(out.Reset이 입력을 먼저 해제하지 않게).
bool Overlaps(const PlanarConstView& in, const DecodedAudioPcm& out) noexcept {
    if (out.ChannelCount() == 0) {
        return false;
    }

    const float* begin = out.Channel(0);
    const float* end = begin + static_cast<usize>(out.FrameCount()) * out.ChannelCount();
    for (uint32 channel = 0; channel < in.channelCount; ++channel) {
        const float* data = in.channels[channel];
        if (data + in.frameCount > begin && data < end) {
            return true;
        }
    }

    return false;
}

Result<void> ResampleInto(const PlanarConstView& in, uint32 sourceRate, uint32 targetRate,
                          Allocator& allocator, DecodedAudioPcm& out) noexcept {
    const uint32 channels = in.channelCount;
    auto converted = CookResampledFrames(in.frameCount, sourceRate, targetRate);
    if (!converted) {
        return Result<void>::Failure(converted.Error());
    }

    // 결과 PCM 상한(원본 디코드와 같은 MaxSoundClipPcmBytes)을 할당 전에 본다.
    const uint64 frames = converted.Value();
    if (frames > MaxSoundClipPcmBytes / (uint64{channels} * sizeof(float))) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    auto planar = out.TryAllocate(channels, targetRate, frames);
    if (!planar) {
        return Result<void>::Failure(planar.Error());
    }

    float* target = planar.Value();
    if (sourceRate == targetRate) {
        for (uint32 channel = 0; channel < channels; ++channel) {
            platform::MemoryCopy(target + static_cast<usize>(channel) * frames,
                                 in.channels[channel], static_cast<usize>(frames) * sizeof(float));
        }

        return Result<void>::Success();
    }

    int error = 0;
    SampleRateState state(channels, error);
    if (state.Get() == nullptr) {
        return Result<void>::Failure(SampleRateError(error));
    }

    // 입력 뒤에 0을 ceil(source / target) + 1 frame 붙인다. libsamplerate의 끝 판정은 floor(N ×
    // 비율) 근처에서 멈추므로, 붙이지 않으면 마지막 frame이 모자라 0이 된다. 붙인 0은 원본 끝 뒤의
    // 무음이라 앞 표본은 그대로다.
    const uint64 padFrames = (sourceRate + targetRate - 1) / targetRate + 1;
    const uint64 totalInput = in.frameCount + padFrames;
    const uint32 outputBlockFrames = InputBlockFrames * MaxRateRatio + OutputBlockSlackFrames;
    Array<float> inputBlock(allocator);
    Array<float> outputBlock(allocator);
    if (auto resized = inputBlock.TryResize(static_cast<usize>(InputBlockFrames) * channels);
        !resized) {
        return resized;
    }

    if (auto resized = outputBlock.TryResize(static_cast<usize>(outputBlockFrames) * channels);
        !resized) {
        return resized;
    }

    const double ratio = static_cast<double>(targetRate) / static_cast<double>(sourceRate);
    uint64 fed = 0;
    uint64 written = 0;
    bool finished = false;
    while (!finished && written < frames) {
        // 다음 입력 블록을 교차로 채운다(원본 끝 뒤는 0).
        const uint64 blockFrames =
            totalInput - fed < InputBlockFrames ? totalInput - fed : InputBlockFrames;
        for (uint64 frame = 0; frame < blockFrames; ++frame) {
            const uint64 at = fed + frame;
            for (uint32 channel = 0; channel < channels; ++channel) {
                inputBlock[static_cast<usize>(frame) * channels + channel] =
                    at < in.frameCount ? in.channels[channel][at] : 0.0f;
            }
        }

        fed += blockFrames;
        const bool endOfInput = fed == totalInput;
        uint64 consumed = 0;
        while (written < frames) {
            SRC_DATA data{};
            data.data_in = inputBlock.Data() + static_cast<usize>(consumed) * channels;
            data.input_frames = static_cast<long>(blockFrames - consumed);
            data.data_out = outputBlock.Data();
            data.output_frames = static_cast<long>(outputBlockFrames);
            data.end_of_input = endOfInput ? 1 : 0;
            data.src_ratio = ratio;
            if (const int processed = src_process(state.Get(), &data); processed != 0) {
                return Result<void>::Failure(SampleRateError(processed));
            }

            consumed += static_cast<uint64>(data.input_frames_used);
            const uint64 produced = static_cast<uint64>(data.output_frames_gen);
            // 남은 입력이 있는데 아무것도 쓰지도 내지도 않으면 진행이 없다(무한 반복 방지).
            if (consumed < blockFrames && data.input_frames_used == 0 && produced == 0) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            const uint64 kept = produced < frames - written ? produced : frames - written;
            for (uint64 frame = 0; frame < kept; ++frame) {
                for (uint32 channel = 0; channel < channels; ++channel) {
                    target[static_cast<usize>(channel) * frames + written + frame] =
                        outputBlock[static_cast<usize>(frame) * channels + channel];
                }
            }

            written += kept;
            // 블록 입력을 다 썼으면 다음 블록으로 간다. 마지막 블록은 더 낼 것이 없을 때 끝난다.
            if (consumed == blockFrames && (!endOfInput || produced == 0)) {
                finished = endOfInput;
                break;
            }
        }
    }

    // 끝 무음을 붙였으므로 libsamplerate는 출력 수 이상을 낸다. 모자라면 libsamplerate 동작이 바뀐
    // 것이다.
    if (written != frames) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

} // namespace

Result<uint64> CookResampledFrames(uint64 frames, uint32 sourceRate, uint32 targetRate) noexcept {
    auto converted = CookResampledPosition(frames, sourceRate, targetRate);
    if (!converted) {
        return converted;
    }

    return Result<uint64>::Success(converted.Value() == 0 ? 1 : converted.Value());
}

Result<uint64> CookResampledPosition(uint64 position, uint32 sourceRate,
                                     uint32 targetRate) noexcept {
    if (!RateValid(sourceRate) || !RateValid(targetRate) || position > MaxConvertedPosition) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<uint64>::Success(RoundRatio(position, sourceRate, targetRate));
}

Result<void> ResampleForCooking(const PlanarConstView& in, uint32 sourceRate, uint32 targetRate,
                                Allocator& allocator, DecodedAudioPcm& out) noexcept {
    if (in.channelCount > MaxOutputChannels || Overlaps(in, out)) {
        // 겹치면 out을 비울 수 없다(입력이 사라진다). out은 그대로 둔다.
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    out.Reset();
    if (in.channelCount == 0 || in.frameCount == 0 || !RateValid(sourceRate) ||
        !RateValid(targetRate)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 channel = 0; channel < in.channelCount; ++channel) {
        for (uint32 frame = 0; frame < in.frameCount; ++frame) {
            if (!math::IsFinite(in.channels[channel][frame])) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    Result<void> resampled = ResampleInto(in, sourceRate, targetRate, allocator, out);
    if (!resampled) {
        out.Reset();
    }

    return resampled;
}

} // namespace rocks::audio
