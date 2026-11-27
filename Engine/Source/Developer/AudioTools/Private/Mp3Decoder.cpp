#include "SourceAudioInternal.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Platform/Memory.h>

#include <minimp3.h>

// Minimp3Glue.c(ThirdParty Minimp3)가 내보내는 minimp3_ex.h tag 함수다.
extern "C" {
size_t RocksMinimp3SkipTags(const uint8_t* buffer, size_t size, size_t* begin);
int RocksMinimp3CheckVbrTag(const uint8_t* frame, int frameBytes, uint32_t* frames, int* delay,
                            int* padding);
}

namespace rocks::audio {

using namespace sourcedetail;

namespace {

// MPEG audio layer 번호다. Layer III만 받는다.
constexpr int32 LayerIII = 3;

// frame 하나가 낼 수 있는 최대 표본 수(교차, 채널 합)다.
constexpr usize MaxFrameSamples = MINIMP3_MAX_SAMPLES_PER_FRAME;

// mp3dec_decode_frame에 한 번에 넘기는 byte 수 상한이다. 인자는 int이고 upstream이 frame 탐색에서
// `i + 2*k` 같은 int 덧셈을 하므로 INT_MAX의 절반으로 둔다(2 GiB 넘는 입력에서 overflow 방지).
constexpr usize MaxCallBytes = static_cast<usize>(NumericLimits<int32>::Max() / 2);

// RocksMinimp3CheckVbrTag 결과다: 0 tag 없음, 1 frame 수·delay·padding 있음, −1 frame 수 없는 tag.
constexpr int32 VbrTagNone = 0;
constexpr int32 VbrTagWithFrames = 1;

int CallBytes(usize remaining) noexcept {
    return static_cast<int>(remaining < MaxCallBytes ? remaining : MaxCallBytes);
}

// 1차 훑기 결과다.
struct Mp3Scan {
    uint32 channels = 0;
    uint32 sampleRate = 0;
    // 채널당 frame 표본 수다. Layer III에서 MPEG-1은 1152, MPEG-2·2.5는 576이고 rate가 고정이라
    // stream 안에서 같다.
    uint32 frameSamples = 0;
    // 처음 frame이 Xing/Info tag frame이면 그 끝 위치다(디코드는 그 뒤에서 시작한다).
    usize audioStart = 0;
    // LAME tag 값이다. delay는 encoder delay + 디코더 지연 529, padding은
    // encoder padding − 529(0 아래면 0)다.
    bool hasTagLength = false;
    uint64 tagFrames = 0;
    uint64 delay = 0;
    uint64 padding = 0;
    // tag frame을 뺀 audio frame들의 채널당 표본 수 합이다.
    uint64 frames = 0;
};

// 첫 frame에서 Xing/Info tag를 읽는다. tag가 있으면 true다.
bool ReadVbrTag(const uint8* frame, int frameBytes, Mp3Scan& scan) noexcept {
    uint32 tagFrames = 0;
    int delay = 0;
    int padding = 0;
    const int tag = RocksMinimp3CheckVbrTag(frame, frameBytes, &tagFrames, &delay, &padding);
    if (tag == VbrTagNone) {
        return false;
    }

    if (tag == VbrTagWithFrames && tagFrames > 0) {
        scan.hasTagLength = true;
        scan.tagFrames = tagFrames;
        scan.delay = delay > 0 ? static_cast<uint64>(delay) : 0;
        scan.padding = padding > 0 ? static_cast<uint64>(padding) : 0;
    }

    return true;
}

// 머리만 읽어 형식과 길이를 센다. 채널·rate가 바뀌면 Unsupported, Layer I·II도 Unsupported다.
Result<void> ScanFrames(const uint8* data, usize size, Mp3Scan& scan) noexcept {
    mp3dec_t decoder;
    mp3dec_init(&decoder);
    usize offset = 0;
    bool first = true;
    while (offset < size) {
        mp3dec_frame_info_t info{};
        const int samples =
            mp3dec_decode_frame(&decoder, data + offset, CallBytes(size - offset), nullptr, &info);
        if (info.frame_bytes <= 0) {
            break;
        }

        const usize frameStart = offset + static_cast<usize>(info.frame_offset);
        offset += static_cast<usize>(info.frame_bytes);
        // channels가 0이면 frame을 찾지 못하고 쓰레기 byte만 건너뛴 것이다.
        if (info.channels <= 0) {
            continue;
        }

        if (info.layer != LayerIII) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        const uint32 channels = static_cast<uint32>(info.channels);
        const uint32 rate = static_cast<uint32>(info.hz);
        if (first) {
            first = false;
            scan.channels = channels;
            scan.sampleRate = rate;
            scan.frameSamples = static_cast<uint32>(samples);
            if (ReadVbrTag(data + frameStart, info.frame_bytes - info.frame_offset, scan)) {
                // tag frame은 무음 frame이라 세지도 디코드하지도 않는다(minimp3_ex와 같다). 디코드
                // 단계가 새 decoder로 이 위치에서 시작하므로, 여기서도 decoder를 비워 같은
                // 방식(frame 탐색)으로 다음 frame을 연다.
                scan.audioStart = offset;
                mp3dec_init(&decoder);
                continue;
            }
        } else if (channels != scan.channels || rate != scan.sampleRate) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        scan.frames += static_cast<uint64>(samples);
    }

    if (first || scan.frames == 0) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

// 디코드한 길이에서 앞뒤로 버릴 frame 수를 정한다(minimp3_ex mp3dec_load와 같은 규칙).
// - tag에 frame 수가 있으면:
//   길이 = min(디코드 − delay, tag frame 수 × frame 표본 수 − delay − padding)
// - 없으면 자르지 않는다.
void TrimRange(const Mp3Scan& scan, uint64 decoded, uint64& skip, uint64& frames) noexcept {
    skip = 0;
    frames = decoded;
    if (!scan.hasTagLength) {
        return;
    }

    skip = scan.delay < decoded ? scan.delay : decoded;
    frames = decoded - skip;
    uint64 detected = scan.tagFrames * scan.frameSamples;
    detected = detected > scan.delay ? detected - scan.delay : 0;
    detected = detected > scan.padding ? detected - scan.padding : 0;
    if (detected > 0 && detected < frames) {
        frames = detected;
    }
}

Result<void> DecodeMp3Into(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    if (file.Size() == 0) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    usize begin = 0;
    const usize size = RocksMinimp3SkipTags(file.Data(), file.Size(), &begin);
    const uint8* data = file.Data() + begin;

    Mp3Scan scan;
    if (auto scanned = ScanFrames(data, size, scan); !scanned) {
        return scanned;
    }

    if (scan.channels > MaxSourceChannels || scan.sampleRate < MinSourceSampleRate ||
        scan.sampleRate > MaxSourceSampleRate) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (ExceedsPcmLimit(scan.channels, scan.frames)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 교차 표본으로 디코드한 뒤 앞뒤를 잘라 planar로 옮긴다.
    Array<float> interleaved(out.GetAllocator());
    if (auto reserved = interleaved.TryReserve(static_cast<usize>(scan.frames * scan.channels));
        !reserved) {
        return reserved;
    }

    const usize frameValues = static_cast<usize>(scan.frameSamples) * scan.channels;
    mp3dec_t decoder;
    mp3dec_init(&decoder);
    float frame[MaxFrameSamples];
    usize offset = scan.audioStart;
    while (offset < size) {
        mp3dec_frame_info_t info{};
        const int samples =
            mp3dec_decode_frame(&decoder, data + offset, CallBytes(size - offset), frame, &info);
        if (info.frame_bytes <= 0) {
            break;
        }

        offset += static_cast<usize>(info.frame_bytes);
        if (info.channels <= 0) {
            continue;
        }

        // 디코드 단계는 깨진 side info 뒤 frame을 다시 찾으므로 1차 훑기와 다른 frame을 볼 수 있다.
        // 형식이 1차 값과 다르면 원본이 손상된 것이다.
        if (info.layer != LayerIII || static_cast<uint32>(info.channels) != scan.channels ||
            static_cast<uint32>(info.hz) != scan.sampleRate) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        // main data가 깨지거나 bit reservoir가 모자라 표본을 내지 못한 frame은 길이를 지키려고
        // 무음으로 채운다.
        if (samples == 0) {
            platform::MemoryZero(frame, frameValues * sizeof(float));
        }

        if (auto appended = interleaved.TryAppend(Span<const float>(frame, frameValues));
            !appended) {
            return appended;
        }
    }

    uint64 skip = 0;
    uint64 frames = 0;
    TrimRange(scan, interleaved.Size() / scan.channels, skip, frames);
    if (frames == 0) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto pcm = out.TryAllocatePcm(scan.channels, scan.sampleRate, frames, SourceSampleFormat::Lossy,
                                  SourceCodec::Mp3);
    if (!pcm) {
        return Result<void>::Failure(pcm.Error());
    }

    float* planar = pcm.Value();
    const float* source = interleaved.Data() + static_cast<usize>(skip) * scan.channels;
    for (uint64 index = 0; index < frames; ++index) {
        for (uint32 channel = 0; channel < scan.channels; ++channel) {
            planar[static_cast<usize>(channel) * frames + index] =
                source[static_cast<usize>(index) * scan.channels + channel];
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> DecodeMp3(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    out.Reset();
    Result<void> decoded = DecodeMp3Into(file, out);
    // 실패하면 부분 결과를 남기지 않는다.
    if (!decoded) {
        out.Reset();
    }

    return decoded;
}

} // namespace rocks::audio
