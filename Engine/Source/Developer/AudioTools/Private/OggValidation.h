#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

// Ogg 원본(Vorbis·Opus)을 디코드하기 전에 물리 stream 전체를 page 단위로 확인한다
// (Docs/Design/AudioSystem 3.28.2절). vorbisfile·opusfile은 CRC가 맞지 않는 page를 건너뛰고,
// link 끝 page나 다음 link의 BOS page가 빠져도 오류 없이 짧게 디코드한다. 무결성은 여기서 본다.
namespace rocks::audio::sourcedetail {

// 확인하는 것:
// - 모든 byte가 CRC가 맞는 page다(앞뒤 쓰레기, 손상 page, 끝의 덜 찬 page는 InvalidArgument).
// - logical stream마다 BOS page로 시작해 page 번호가 이어지고 EOS page로 끝난다.
// - stream은 차례로 이어진다(연결, chained). 한 stream이 열린 채 다른 BOS가 오면
//   (다중화) Unsupported다.
// - serial이 같은 stream이 다시 오면 InvalidArgument다(규격 위반).
// 오류: InvalidArgument(위 위반, page 없음), Unsupported(다중화), OutOfMemory.
Result<void> ValidateOggPages(Span<const uint8> file, Allocator& allocator) noexcept;

} // namespace rocks::audio::sourcedetail
