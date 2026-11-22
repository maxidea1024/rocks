#include "SourceAudioInternal.h"

#include <Core/Move.h>
#include <Platform/Memory.h>

namespace rocks::audio {

void DecodedSourceAudio::Reset() noexcept {
    pcm_.Clear();
    loops_.Clear();
    cues_.Clear();
    channels_ = 0;
    sampleRate_ = 0;
    frames_ = 0;
    sampleFormat_ = SourceSampleFormat::Int16;
    codec_ = SourceCodec::Pcm;
    ignoredChunks_ = 0;
}

Result<float*> DecodedSourceAudio::TryAllocatePcm(uint32 channels, uint32 sampleRate, uint64 frames,
                                                  SourceSampleFormat format,
                                                  SourceCodec codec) noexcept {
    if (sourcedetail::ExceedsPcmLimit(channels, frames)) {
        return Result<float*>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto resized = pcm_.TryResize(static_cast<usize>(frames * channels)); !resized) {
        return Result<float*>::Failure(resized.Error());
    }

    channels_ = channels;
    sampleRate_ = sampleRate;
    frames_ = frames;
    sampleFormat_ = format;
    codec_ = codec;
    return Result<float*>::Success(pcm_.Data());
}

Result<void> DecodedSourceAudio::TryAddLoop(SourceLoop loop) noexcept {
    return loops_.TryPushBack(loop);
}

Result<void> DecodedSourceAudio::TryAddCue(uint32 id, uint64 frame) noexcept {
    SourceCue cue(*allocator_);
    cue.id = id;
    cue.frame = frame;
    return cues_.TryPushBack(Move(cue));
}

Result<void> DecodedSourceAudio::TryNameCue(uint32 id, StringView name) noexcept {
    for (usize index = 0; index < cues_.Size(); ++index) {
        if (cues_[index].id == id) {
            if (!name.IsValidUtf8()) {
                return Result<void>::Success();
            }

            return cues_[index].name.TryAssign(name);
        }
    }

    return Result<void>::Success();
}

namespace sourcedetail {

SourceSampleFormat FormatOf(uint32 bytesPerSample, SampleEncoding encoding) noexcept {
    if (encoding == SampleEncoding::Float) {
        return bytesPerSample == sizeof(double) ? SourceSampleFormat::Float64
                                                : SourceSampleFormat::Float32;
    }

    switch (bytesPerSample) {
    case 1:
        return encoding == SampleEncoding::UnsignedInt ? SourceSampleFormat::UInt8
                                                       : SourceSampleFormat::Int16;
    case 2:
        return SourceSampleFormat::Int16;
    case 3:
        return SourceSampleFormat::Int24;
    default:
        return SourceSampleFormat::Int32;
    }
}

namespace {

float ConvertSample(const uint8* bytes, uint32 bytesPerSample, SampleEncoding encoding,
                    bool bigEndian) noexcept {
    if (encoding == SampleEncoding::Float) {
        if (bytesPerSample == sizeof(double)) {
            const uint64 high = ReadU32(bytes + (bigEndian ? 0 : 4), bigEndian);
            const uint64 low = ReadU32(bytes + (bigEndian ? 4 : 0), bigEndian);
            return static_cast<float>(platform::BitCast<double>((high << 32) | low));
        }

        return platform::BitCast<float>(ReadU32(bytes, bigEndian));
    }

    const uint32 raw = ReadUnsigned(bytes, bytesPerSample, bigEndian);
    const uint32 bits = bytesPerSample * BitsPerByte;
    if (encoding == SampleEncoding::UnsignedInt) {
        return static_cast<float>(static_cast<double>(static_cast<int32>(raw) - UnsignedByteBias) /
                                  Int8Scale);
    }

    // 부호 확장: 상위 bit를 맞춘 뒤 산술 시프트한다.
    const int32 shift = static_cast<int32>(32 - bits);
    const int32 value = static_cast<int32>(raw << shift) >> shift;
    double scale = Int32Scale;
    if (bits == 8) {
        scale = Int8Scale;
    } else if (bits == 16) {
        scale = Int16Scale;
    } else if (bits == 24) {
        scale = Int24Scale;
    }

    return static_cast<float>(static_cast<double>(value) / scale);
}

} // namespace

void DeinterleaveToFloat(const uint8* data, uint32 channels, uint64 frames, uint32 bytesPerSample,
                         SampleEncoding encoding, bool bigEndian, float* planar) noexcept {
    const usize frameBytes = static_cast<usize>(channels) * bytesPerSample;
    for (uint64 frame = 0; frame < frames; ++frame) {
        const uint8* frameData = data + static_cast<usize>(frame) * frameBytes;
        for (uint32 channel = 0; channel < channels; ++channel) {
            planar[static_cast<usize>(channel) * frames + frame] =
                ConvertSample(frameData + static_cast<usize>(channel) * bytesPerSample,
                              bytesPerSample, encoding, bigEndian);
        }
    }
}

} // namespace sourcedetail

} // namespace rocks::audio
