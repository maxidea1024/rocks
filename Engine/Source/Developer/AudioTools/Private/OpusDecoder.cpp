#include "OggValidation.h"
#include "SourceAudioInternal.h"

#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

#include <opusfile.h>

namespace rocks::audio {

using namespace sourcedetail;

namespace {

// Opus는 언제나 48 kHz로 디코드한다(원본 입력 rate는 머리의 참고값일 뿐이다).
constexpr uint32 OpusOutputRate = 48000;

// op_read_float 한 번에 받는 채널당 표본 수다(Opus packet 최대 120 ms × 48 kHz).
constexpr int OpusMaxReadSamples = 5760;

// 길이를 알 때 미리 잡는 교차 표본 수 상한이다(16M float = 64 MiB, FLAC과 같다).
constexpr usize OpusMaxPreReservedSamples = usize{16} * 1024 * 1024;

// op_read_float 결과 0은 stream 끝이다.
constexpr int OpusEndOfStream = 0;

// op_pcm_total에 넘기는 "모든 link" 값이다.
constexpr int OpusAllLinks = -1;

// OggOpusFile 수명이다.
class OpusFileHandle {
public:
    explicit OpusFileHandle(OggOpusFile* file) noexcept : file_(file) {
    }

    ~OpusFileHandle() {
        if (file_ != nullptr) {
            op_free(file_);
        }
    }

    OpusFileHandle(const OpusFileHandle&) = delete;
    OpusFileHandle& operator=(const OpusFileHandle&) = delete;

    OggOpusFile* Get() const noexcept {
        return file_;
    }

private:
    OggOpusFile* file_;
};

// OpusHead(RFC 7845 5.1절)를 열기 전에 본다. opusfile은 지원하지 않는 형식도 머리
// 손상(OP_EBADHEADER)으로 돌려주므로, mapping family 2·3(ambisonics)·255와 9채널 이상을 여기서
// Unsupported로 가린다. page 구조는 이미 확인했고 OpusHead는 첫 page 하나를 혼자 채운다. 표지가
// 다르면 opusfile에 맡긴다(OP_ENOTFORMAT).
Result<void> CheckOpusHead(Span<const uint8> file) noexcept {
    constexpr usize PageHeaderBytes = 27;
    constexpr usize SegmentCountOffset = 26;
    constexpr usize HeadMagicBytes = 8;
    constexpr usize HeadChannelsOffset = 9;
    constexpr usize HeadFamilyOffset = 18;
    constexpr usize HeadMinimumBytes = 19;
    constexpr uint8 FamilyMonoStereo = 0;
    constexpr uint8 FamilyVorbisOrder = 1;
    constexpr char HeadMagic[HeadMagicBytes + 1] = "OpusHead";
    const usize body = PageHeaderBytes + file[SegmentCountOffset];
    if (file.Size() < body + HeadMinimumBytes) {
        return Result<void>::Success();
    }

    const uint8* head = file.Data() + body;
    for (usize index = 0; index < HeadMagicBytes; ++index) {
        if (head[index] != static_cast<uint8>(HeadMagic[index])) {
            return Result<void>::Success();
        }
    }

    const uint8 family = head[HeadFamilyOffset];
    if ((family != FamilyMonoStereo && family != FamilyVorbisOrder) ||
        head[HeadChannelsOffset] > MaxSourceChannels) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

// opusfile 오류를 Rocks 오류로 옮긴다: 할당 실패는 OutOfMemory, 지원하지 않는 기능·판은
// Unsupported, 그 밖은 원본 손상이다.
ErrorCode OpusErrorCode(int error) noexcept {
    if (error == OP_EFAULT) {
        return ErrorCode::OutOfMemory;
    }

    return error == OP_EIMPL || error == OP_EVERSION ? ErrorCode::Unsupported
                                                     : ErrorCode::InvalidArgument;
}

Result<void> DecodeOpusInto(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    if (file.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // opusfile도 CRC가 틀린 page와 빠진 link 끝·BOS page를 건너뛸 수 있으므로 page 구조를 먼저
    // 확인한다(3.28.2절).
    if (auto valid = ValidateOggPages(file, out.GetAllocator()); !valid) {
        return valid;
    }

    if (auto head = CheckOpusHead(file); !head) {
        return head;
    }

    int openError = 0;
    OpusFileHandle opus(op_open_memory(file.Data(), file.Size(), &openError));
    if (opus.Get() == nullptr) {
        return Result<void>::Failure(OpusErrorCode(openError));
    }

    // 연결된 stream은 link마다 채널 수가 다를 수 있다. 모두 첫 link와 같아야 한다.
    const int firstChannels = op_channel_count(opus.Get(), 0);
    const int links = op_link_count(opus.Get());
    for (int link = 1; link < links; ++link) {
        if (op_channel_count(opus.Get(), link) != firstChannels) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    if (firstChannels <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (static_cast<uint32>(firstChannels) > MaxSourceChannels) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 메모리 stream은 seekable이라 길이(granule 기준, pre-skip·끝 자르기 반영)를 미리 안다. 디코드
    // 전에 상한을 판정하고, 끝에 디코드한 길이와 맞춰 본다(TOC가 잘못된 packet은 opusfile이 오류
    // 없이 버리기 때문이다).
    const uint32 channels = static_cast<uint32>(firstChannels);
    const ogg_int64_t total = op_pcm_total(opus.Get(), OpusAllLinks);
    if (total <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 expectedFrames = static_cast<uint64>(total);
    if (ExceedsPcmLimit(channels, expectedFrames)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 교차 표본으로 쌓고 끝에 planar로 옮긴다. 미리 잡는 양은
    // OpusMaxPreReservedSamples까지다(손상된 granule이 큰 버퍼를 잡지 않게).
    const usize readValues = static_cast<usize>(OpusMaxReadSamples) * channels;
    const usize declared = static_cast<usize>(expectedFrames * channels);
    Array<float> interleaved(out.GetAllocator());
    if (auto reserved = interleaved.TryReserve(
            declared < OpusMaxPreReservedSamples ? declared : OpusMaxPreReservedSamples);
        !reserved) {
        return reserved;
    }

    while (true) {
        const usize before = interleaved.Size();
        // 미리 안 길이를 넘는 출력은 손상이다(상한도 그 길이로 이미 판정했다).
        if (before / channels > expectedFrames) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto space = interleaved.TryAppendUninitialized(readValues);
        if (!space) {
            return Result<void>::Failure(space.Error());
        }

        const int read =
            op_read_float(opus.Get(), space.Value().Data(), static_cast<int>(readValues), nullptr);
        if (read < 0) {
            // page 구조는 위에서 확인했으므로 여기 오는 것은 packet 손상(또는 link 전환 중 할당
            // 실패)이다.
            return Result<void>::Failure(OpusErrorCode(read));
        }

        const usize produced = static_cast<usize>(read) * channels;
        if (auto shrunk = interleaved.TryResize(before + produced); !shrunk) {
            return shrunk;
        }

        if (read == OpusEndOfStream) {
            break;
        }

        // 손상된 packet이 비유한 값을 낼 수 있다. 믹서까지 가지 않게 막는다.
        for (usize index = before; index < before + produced; ++index) {
            if (!math::IsFinite(interleaved[index])) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    const uint64 frames = interleaved.Size() / channels;
    if (frames != expectedFrames) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto pcm = out.TryAllocatePcm(channels, OpusOutputRate, frames, SourceSampleFormat::Lossy,
                                  SourceCodec::Opus);
    if (!pcm) {
        return Result<void>::Failure(pcm.Error());
    }

    // mapping family 0·1은 Vorbis 채널 순서다. 내부 순서로 바꾸며 planar로 옮긴다(3.3절).
    DeinterleaveVorbisOrder(interleaved.Data(), channels, frames, pcm.Value());
    return Result<void>::Success();
}

} // namespace

Result<void> DecodeOpus(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    out.Reset();
    Result<void> decoded = DecodeOpusInto(file, out);
    // 실패하면 부분 결과를 남기지 않는다.
    if (!decoded) {
        out.Reset();
    }

    return decoded;
}

} // namespace rocks::audio
