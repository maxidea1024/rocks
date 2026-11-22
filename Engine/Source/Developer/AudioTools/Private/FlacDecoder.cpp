#include "SourceAudioInternal.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>

#include <FLAC/stream_decoder.h>

namespace rocks::audio {

using namespace sourcedetail;

namespace {

// 길이를 알 때 디코드 전에 미리 잡는 교차 표본 수 상한이다(16M float = 64 MiB). STREAMINFO 길이는
// CRC가 없는 숫자 하나라, 손상·조작된 작은 파일이 큰 버퍼를 잡지 않게 한다. 나머지는 쌓으며 늘린다.
constexpr usize MaxPreReservedSamples = usize{16} * 1024 * 1024;

// 디코드 중 상태다. callback이 client data로 받는다.
struct FlacContext {
    explicit FlacContext(Allocator& allocator) noexcept : interleaved(allocator) {
    }

    const uint8* data = nullptr;
    usize size = 0;
    usize position = 0;

    // STREAMINFO 값이다. totalFrames가 0이면 길이를 모른다(파이프로 쓴 파일).
    bool hasStreamInfo = false;
    uint32 channels = 0;
    uint32 sampleRate = 0;
    uint32 bits = 0;
    uint64 totalFrames = 0;

    // 첫 write에서 STREAMINFO를 확인했는지다.
    bool streamChecked = false;
    // LOST_SYNC를 받았는지다. 그 뒤 audio가 오면(libFLAC이 채운 무음 frame 포함) 손상이다.
    bool lostSync = false;

    Array<float> interleaved;
    // 실패 이유다. Success가 아니면 write callback이 디코드를 멈춘다.
    ErrorCode failure = ErrorCode::None;
};

FLAC__StreamDecoderReadStatus ReadCallback(const FLAC__StreamDecoder*, FLAC__byte buffer[],
                                           size_t* bytes, void* clientData) {
    FlacContext& context = *static_cast<FlacContext*>(clientData);
    const usize remaining = context.size - context.position;
    if (remaining == 0) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    const usize count = *bytes < remaining ? *bytes : remaining;
    platform::MemoryCopy(buffer, context.data + context.position, count);
    context.position += count;
    *bytes = count;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__bool EofCallback(const FLAC__StreamDecoder*, void* clientData) {
    const FlacContext& context = *static_cast<const FlacContext*>(clientData);
    return context.position >= context.size;
}

void MetadataCallback(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata,
                      void* clientData) {
    FlacContext& context = *static_cast<FlacContext*>(clientData);
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
        return;
    }

    // STREAMINFO가 둘이면 libFLAC은 뒤 값(MD5 등)을 쓴다. 어느 쪽인지 모호하므로 거부한다.
    if (context.hasStreamInfo) {
        context.failure = ErrorCode::InvalidArgument;
        return;
    }

    const FLAC__StreamMetadata_StreamInfo& info = metadata->data.stream_info;
    context.hasStreamInfo = true;
    context.channels = info.channels;
    context.sampleRate = info.sample_rate;
    context.bits = info.bits_per_sample;
    context.totalFrames = info.total_samples;
}

// STREAMINFO가 받을 수 있는 형식인지 보고, 길이를 알면 PCM 상한을 디코드 전에 판정한다.
ErrorCode CheckStreamInfo(FlacContext& context) noexcept {
    if (!context.hasStreamInfo || context.channels == 0 ||
        context.bits < FLAC__MIN_BITS_PER_SAMPLE || context.bits > FLAC__MAX_BITS_PER_SAMPLE) {
        return ErrorCode::InvalidArgument;
    }

    if (context.channels > MaxSourceChannels || context.sampleRate < MinSourceSampleRate ||
        context.sampleRate > MaxSourceSampleRate) {
        return ErrorCode::Unsupported;
    }

    if (ExceedsPcmLimit(context.channels, context.totalFrames)) {
        return ErrorCode::CapacityExceeded;
    }

    if (context.totalFrames > 0) {
        const usize declared = static_cast<usize>(context.totalFrames * context.channels);
        if (!context.interleaved.TryReserve(
                declared < MaxPreReservedSamples ? declared : MaxPreReservedSamples)) {
            return ErrorCode::OutOfMemory;
        }
    }

    return ErrorCode::None;
}

FLAC__StreamDecoderWriteStatus WriteCallback(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                             const FLAC__int32* const buffer[], void* clientData) {
    FlacContext& context = *static_cast<FlacContext*>(clientData);
    if (context.failure == ErrorCode::None && !context.streamChecked) {
        context.streamChecked = true;
        context.failure = CheckStreamInfo(context);
    }

    // sync를 잃은 뒤 audio가 왔다: frame 안 손상(subframe 머리·residual 오류)도 LOST_SYNC로
    // 보고되고, libFLAC은 빠진 구간을 무음 frame으로 채워 길이를 맞춘다. 마지막 frame 뒤
    // 쓰레기(끝의 ID3v1)는 뒤따르는 write가 없으므로 받는다.
    if (context.failure == ErrorCode::None && context.lostSync) {
        context.failure = ErrorCode::InvalidArgument;
    }

    if (context.failure != ErrorCode::None) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // frame마다 형식이 STREAMINFO와 같아야 한다(한 SoundClip은 형식이 하나다).
    const FLAC__FrameHeader& header = frame->header;
    if (header.channels != context.channels || header.bits_per_sample != context.bits ||
        header.sample_rate != context.sampleRate) {
        context.failure = ErrorCode::Unsupported;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // frame이 이어져야 한다(시작 표본 번호 = 지금까지 frame 수). 무음 채움 조건이 맞지 않아 생긴
    // 구멍을 길이를 모를 때도 잡는다.
    const uint64 decoded = context.interleaved.Size() / context.channels;
    if (header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER ||
        header.number.sample_number != decoded) {
        context.failure = ErrorCode::InvalidArgument;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    const uint64 frames = decoded + header.blocksize;
    // 길이를 알면 그보다 길 수 없고, 모르면 쌓는 동안 상한을 본다.
    if (context.totalFrames > 0 && frames > context.totalFrames) {
        context.failure = ErrorCode::InvalidArgument;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    if (ExceedsPcmLimit(context.channels, frames)) {
        context.failure = ErrorCode::CapacityExceeded;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    auto appended = context.interleaved.TryAppendUninitialized(
        static_cast<usize>(header.blocksize) * context.channels);
    if (!appended) {
        context.failure = appended.Error();
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // 정수 표본을 2^(bits−1)로 나눈다. 32비트 이하 정수는 double에서 정확하고, 24비트 이하는
    // float32에서도 정확하다.
    const double scale = 1.0 / static_cast<double>(uint64{1} << (context.bits - 1));
    float* out = appended.Value().Data();
    for (uint32 index = 0; index < header.blocksize; ++index) {
        for (uint32 channel = 0; channel < context.channels; ++channel) {
            *out++ = static_cast<float>(static_cast<double>(buffer[channel][index]) * scale);
        }
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void ErrorCallback(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status,
                   void* clientData) {
    FlacContext& context = *static_cast<FlacContext*>(clientData);
    // LOST_SYNC는 표시만 한다: 그 뒤 audio가 오면 write callback이 거부하고, 마지막 frame 뒤
    // 쓰레기(끝의 ID3v1 등)면 받는다. 첫 frame 앞(ID3v2 뒤 padding, ID3v2.4 footer 등)은 표시하지
    // 않는다 — libFLAC은 첫 frame 전에는 무음 채움을 하지 않고, 빠진 첫 frame은 frame 번호 연속
    // 검사가 잡는다. 그 밖(CRC 불일치·머리 오류· 해석 불가·범위 밖·빠진 frame)은 원본 손상이라 바로
    // 거부한다. libFLAC은 이때 무음을 넣고 계속하지만, 무손실 원본을 조용히 바꿔 임포트하지 않는다.
    if (status == FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC) {
        if (context.interleaved.Size() > 0) {
            context.lostSync = true;
        }
    } else if (context.failure == ErrorCode::None) {
        context.failure = ErrorCode::InvalidArgument;
    }
}

// FLAC 표본 bit 수를 저장 형식으로 옮긴다(8비트 이하는 Int16으로 넓혀 보고한다 — AIFF와 같다).
SourceSampleFormat FlacFormatOf(uint32 bits) noexcept {
    constexpr uint32 Int16Bits = 16;
    constexpr uint32 Int24Bits = 24;
    if (bits <= Int16Bits) {
        return SourceSampleFormat::Int16;
    }

    return bits <= Int24Bits ? SourceSampleFormat::Int24 : SourceSampleFormat::Int32;
}

// FLAC 디코더 객체 수명이다.
class FlacDecoderHandle {
public:
    FlacDecoderHandle() noexcept : decoder_(FLAC__stream_decoder_new()) {
    }

    ~FlacDecoderHandle() {
        if (decoder_ != nullptr) {
            FLAC__stream_decoder_delete(decoder_);
        }
    }

    FlacDecoderHandle(const FlacDecoderHandle&) = delete;
    FlacDecoderHandle& operator=(const FlacDecoderHandle&) = delete;

    FLAC__StreamDecoder* Get() const noexcept {
        return decoder_;
    }

private:
    FLAC__StreamDecoder* decoder_;
};

Result<void> DecodeFlacInto(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    FlacDecoderHandle decoder;
    if (decoder.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    FlacContext context(out.GetAllocator());
    context.data = file.Data();
    context.size = file.Size();
    // STREAMINFO의 MD5가 있으면(0이 아니면) 디코드 결과와 맞춰 본다.
    FLAC__stream_decoder_set_md5_checking(decoder.Get(), true);
    const FLAC__StreamDecoderInitStatus initialized = FLAC__stream_decoder_init_stream(
        decoder.Get(), ReadCallback, nullptr, nullptr, nullptr, EofCallback, WriteCallback,
        MetadataCallback, ErrorCallback, &context);
    if (initialized != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        return Result<void>::Failure(
            initialized == FLAC__STREAM_DECODER_INIT_STATUS_MEMORY_ALLOCATION_ERROR
                ? ErrorCode::OutOfMemory
                : ErrorCode::InvalidArgument);
    }

    const bool processed = FLAC__stream_decoder_process_until_end_of_stream(decoder.Get()) != 0;
    const FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(decoder.Get());
    // finish가 MD5 불일치면 false다.
    const bool finished = FLAC__stream_decoder_finish(decoder.Get()) != 0;
    if (context.failure != ErrorCode::None) {
        return Result<void>::Failure(context.failure);
    }

    if (state == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (!processed || !finished || !context.hasStreamInfo || context.interleaved.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 frames = context.interleaved.Size() / context.channels;
    // 길이를 알면 끝까지 있어야 한다(잘린 파일).
    if (context.totalFrames > 0 && frames != context.totalFrames) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto pcm = out.TryAllocatePcm(context.channels, context.sampleRate, frames,
                                  FlacFormatOf(context.bits), SourceCodec::Flac);
    if (!pcm) {
        return Result<void>::Failure(pcm.Error());
    }

    float* planar = pcm.Value();
    const float* source = context.interleaved.Data();
    for (uint64 index = 0; index < frames; ++index) {
        for (uint32 channel = 0; channel < context.channels; ++channel) {
            planar[static_cast<usize>(channel) * frames + index] =
                source[static_cast<usize>(index) * context.channels + channel];
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> DecodeFlac(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    out.Reset();
    Result<void> decoded = DecodeFlacInto(file, out);
    // 실패하면 부분 결과를 남기지 않는다.
    if (!decoded) {
        out.Reset();
    }

    return decoded;
}

} // namespace rocks::audio
