#pragma once

#include <AudioTools/AudioEncoder.h>

// 인코더 출력을 청크로 나눠 플랫폼 데이터 byte를 만든다
// (Docs/Design/AudioSystem 3.30.1·3.30.3절).
//
// 사용 예:
//   AudioPlatformDataOptions options;
//   options.targetChunkBytes = 64 * 1024;
//   Array<uint8> bytes(allocator);
//   if (auto written = WriteAudioPlatformData(encoded, options, bytes); !written) { ... }
namespace rocks::audio {

// 목표 청크 byte 기본값이다(256 KiB).
inline constexpr uint32 TargetChunkBytes = 256 * 1024;

struct AudioPlatformDataOptions {
    // 청크 자기 단위 byte 목표(pre-roll 복제분 제외)다. 단위 하나가 목표보다 크면 그 청크만 넘는다.
    uint32 targetChunkBytes = TargetChunkBytes;
    // 첫 청크 목표다. 0이면 targetChunkBytes다(UE ZerothChunkSize에 해당 — 상주 첫 청크를 작게).
    uint32 firstChunkBytes = 0;
    // loop 구간(유효 frame 기준, loopEnd 0이면 없음)이다.
    uint64 loopStart = 0;
    uint64 loopEnd = 0;
};

// encoded를 청크로 나눠 out에 플랫폼 데이터 byte([메타][payload 영역])를 쓴다(out은 비우고 다시
// 채운다).
// - 청크 k > 0 앞에는 바로 앞 단위들을 frame 합이 encoded.preRollFrames 이상이 될 때까지 복제한다
//   (codec frame 0 앞으로는 넘지 않는다).
// - encoded가 형식 규칙(채널·마스크·rate, 단위 frame 합 = delay + 유효 + padding, 단위 구간,
//   codec 머리 8 배수, PCM16·ADPCM은 pre-roll·codec 머리 0)이나 loop 구간에 맞지 않거나 목표가
//   0이면 InvalidArgument, 청크 하나가 4 GiB(청크 표 32비트 칸)를 넘으면 CapacityExceeded, 할당
//   실패는 OutOfMemory다(실패하면 out은 비어 있다).
// 같은 입력이면 같은 byte다.
ROCKS_AUDIOTOOLS_API Result<void> WriteAudioPlatformData(const EncodedAudio& encoded,
                                                         const AudioPlatformDataOptions& options,
                                                         Array<uint8>& out) noexcept;

} // namespace rocks::audio
