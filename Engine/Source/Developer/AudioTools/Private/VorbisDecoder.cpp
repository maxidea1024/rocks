#include "OggValidation.h"
#include "SourceAudioInternal.h"

#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

#include <vorbis/vorbisfile.h>

namespace rocks::audio {

using namespace sourcedetail;

namespace {

// ov_read_float 한 번에 받는 채널당 표본 수 상한이다(vorbisfile이 한 packet 안에서 나눠 준다).
constexpr int MaxReadSamples = 4096;

// ov_read_float 결과 0은 stream 끝이다.
constexpr long EndOfStream = 0;

// ov_info에 넘기는 "지금 link" 값이다.
constexpr int CurrentLink = -1;

// 메모리 원본을 vorbisfile callback으로 읽는 상태다.
struct MemorySource {
    const uint8* data = nullptr;
    usize size = 0;
    usize position = 0;
};

size_t VorbisReadCallback(void* buffer, size_t elementSize, size_t count, void* source) {
    MemorySource& memory = *static_cast<MemorySource*>(source);
    if (elementSize == 0) {
        return 0;
    }

    const usize remaining = memory.size - memory.position;
    const usize elements = count < remaining / elementSize ? count : remaining / elementSize;
    platform::MemoryCopy(buffer, memory.data + memory.position, elements * elementSize);
    memory.position += elements * elementSize;
    return elements;
}

// OggVorbis_File 수명이다. open이 성공했을 때만 ov_clear를 부른다(실패하면 vorbisfile이 스스로
// 정리한다).
class VorbisFileHandle {
public:
    VorbisFileHandle() noexcept {
        platform::MemoryZero(&file_, sizeof(file_));
    }

    ~VorbisFileHandle() {
        if (open_) {
            ov_clear(&file_);
        }
    }

    VorbisFileHandle(const VorbisFileHandle&) = delete;
    VorbisFileHandle& operator=(const VorbisFileHandle&) = delete;

    // read callback만 주어 순차(non-seekable)로 연다. seekable로 열면 vorbisfile 1.3.7이
    // 연결(chained) stream의 첫 link를 건너뛰는 경우가 있다(3.28.2절). close callback은 두지
    // 않는다(메모리는 호출자 소유다).
    bool Open(MemorySource& source) noexcept {
        const ov_callbacks callbacks = {VorbisReadCallback, nullptr, nullptr, nullptr};
        open_ = ov_open_callbacks(&source, &file_, nullptr, 0, callbacks) == 0;
        return open_;
    }

    OggVorbis_File* Get() noexcept {
        return &file_;
    }

private:
    OggVorbis_File file_;
    bool open_ = false;
};

Result<void> DecodeVorbisInto(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    if (file.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // vorbisfile은 CRC가 틀린 page와 빠진 link 끝·BOS page를 조용히 건너뛰므로 page 구조를 먼저
    // 확인한다.
    if (auto valid = ValidateOggPages(file, out.GetAllocator()); !valid) {
        return valid;
    }

    MemorySource source{file.Data(), file.Size(), 0};
    VorbisFileHandle vorbis;
    if (!vorbis.Open(source)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    OggVorbis_File* handle = vorbis.Get();
    const vorbis_info* first = ov_info(handle, CurrentLink);
    if (first == nullptr || first->channels <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const int firstChannels = first->channels;
    const long rate = first->rate;
    if (static_cast<uint32>(firstChannels) > MaxSourceChannels ||
        rate < static_cast<long>(MinSourceSampleRate) ||
        rate > static_cast<long>(MaxSourceSampleRate)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 순차로 읽으므로 길이를 미리 모른다. 교차 표본으로 쌓으며 상한을 보고, 끝에 planar로 옮긴다.
    const uint32 channels = static_cast<uint32>(firstChannels);
    Array<float> interleaved(out.GetAllocator());
    // 순차 모드의 vorbisfile은 연결된 stream의 link 경계를 지날 때 OV_HOLE을 한 번 돌려준다. 그래서
    // OV_HOLE을 받으면 표시만 하고, 다음 읽기가 새 link에서 나오면 경계로 본다.
    int currentLink = 0;
    bool pendingHole = false;
    while (true) {
        float** buffers = nullptr;
        int link = 0;
        const long read = ov_read_float(handle, &buffers, MaxReadSamples, &link);
        if (read == EndOfStream) {
            // 끝 앞의 OV_HOLE은 경계가 아니라 마지막 page 손상·누락이다.
            if (pendingHole) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            break;
        }

        if (read == OV_HOLE && !pendingHole) {
            pendingHole = true;
            continue;
        }

        // 연이은 OV_HOLE과 그 밖 오류는 원본 손상이다. 손실 원본이라도 빠진 구간을 메우지 않고
        // 거부한다 (Ogg page는 CRC가 있어 손상을 정확히 안다).
        if (read < 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // OV_HOLE 뒤에도 같은 link면 link 안의 page 손상·누락이다.
        if (pendingHole && link == currentLink) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        pendingHole = false;
        currentLink = link;

        // 연결된 stream은 link마다 형식이 다를 수 있다. 모두 첫 link와 같아야 한다.
        const vorbis_info* info = ov_info(handle, CurrentLink);
        if (info == nullptr || info->channels != firstChannels || info->rate != rate) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        const uint64 frames = interleaved.Size() / channels + static_cast<uint64>(read);
        if (ExceedsPcmLimit(channels, frames)) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        auto appended = interleaved.TryAppendUninitialized(static_cast<usize>(read) * channels);
        if (!appended) {
            return Result<void>::Failure(appended.Error());
        }

        float* target = appended.Value().Data();
        for (long index = 0; index < read; ++index) {
            for (uint32 channel = 0; channel < channels; ++channel) {
                const float sample = buffers[channel][index];
                // 손상된 setup·packet이 비유한 값을 낼 수 있다. 믹서까지 가지 않게 막는다.
                if (!math::IsFinite(sample)) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                *target++ = sample;
            }
        }
    }

    const uint64 frames = interleaved.Size() / channels;
    if (frames == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto pcm = out.TryAllocatePcm(channels, static_cast<uint32>(rate), frames,
                                  SourceSampleFormat::Lossy, SourceCodec::Vorbis);
    if (!pcm) {
        return Result<void>::Failure(pcm.Error());
    }

    // Vorbis 채널 순서를 내부 순서로 바꾸며 planar로 옮긴다(3.3절).
    DeinterleaveVorbisOrder(interleaved.Data(), channels, frames, pcm.Value());

    return Result<void>::Success();
}

} // namespace

Result<void> DecodeVorbis(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    out.Reset();
    Result<void> decoded = DecodeVorbisInto(file, out);
    // 실패하면 부분 결과를 남기지 않는다.
    if (!decoded) {
        out.Reset();
    }

    return decoded;
}

} // namespace rocks::audio
