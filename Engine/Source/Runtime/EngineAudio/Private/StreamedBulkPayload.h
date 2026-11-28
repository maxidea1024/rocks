#pragma once

#include <AssetRuntime/StreamedBulk.h>
#include <AudioAssets/SoundStreamedPayload.h>
#include <Core/Memory/SharedPtr.h>

// SoundStreamedPayload의 엔진 구현이다(Docs/Design/AudioSystem 3.46.3절): AssetManager::
// OpenStreamedBulk가 준 StreamedBulk를 감싼다. 즉시 실패·callback 정확히 한 번·Pending 손잡이
// 소멸 Fatal을 StreamedBulk 그대로 옮긴다(손잡이가 BulkReadHandle을 쥔다).
//
// 수명: allocator는 이 객체와 진행 중 읽기보다 오래 살고 다른 스레드의 해제를 지원해야 한다(Read가
// 아무 스레드에서 할당하고 callback 스레드가 놓는다). StreamedBulk와 같이 AssetManager 파괴 전에
// 놓아야 한다.
namespace rocks::engine_audio_detail {

// bulk를 감싼 payload를 만든다. 오류: 빈 bulk InvalidArgument, 할당 실패.
Result<SharedPtr<SoundStreamedPayload>> MakeStreamedBulkPayload(Allocator& allocator,
                                                                StreamedBulk bulk) noexcept;

} // namespace rocks::engine_audio_detail
