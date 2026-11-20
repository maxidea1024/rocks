#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// transient heap·placed 자원·aliasing 획득(RenderBase 8절 GX12)을 실제 장치로 확인한다.
// - Targets heap 하나에 RGBA8 렌더 타깃(32×32) A·B를 같은 offset 0에, C를 겹치지 않는 offset에
//   둔다. A를 attachment 획득으로 빨강 clear하고 읽는다. 다음 기록에서 C를 명시 획득 전이
//   (ResourceTransition::acquire)로 받아 A를 복사한다. 그 다음 B를 attachment 획득으로 파랑 clear한
//   뒤 B는 파랑, C는 그대로 빨강인지 읽는다.
// - Buffers heap 하나에 버퍼 X·Y를 같은 offset 0에 둔다. 각각 획득 전이 뒤 서로 다른 원본에서
//   CopyBuffer로 채워 ReadBuffer로 대조하고, Y를 쓴 뒤 X가 같은 메모리(Y 내용)를 읽는지 본다.
// - 거부: 크기 0 heap, Sampled 텍스처 placement, 정렬이 맞지 않는 offset, heap 밖 offset, 종류가
//   다른 heap(InvalidArgument).
// - 검증 오류 증가 0, 모두 회수(Targets heap은 placed 자원보다 먼저, Buffers heap은 나중에 놓는다)
//   하고 WaitIdle 뒤 PendingRetireCount가 처음 값으로 돌아온다.
// DeviceFeatures::transientHeaps가 없으면 건너뛴다. device는 호출 스레드가 소유한다. 실패 수를
// 돌려준다.
rocks::uint32 RunTransientHeapChecks(const char* name, rocks::graphics::Device& device,
                                     rocks::Allocator& allocator) noexcept;

} // namespace sandbox
