#pragma once

#include "SampleRenderer.h"

namespace sandbox {

// 스왑체인 공유(RenderBase 8절 "스왑체인 공유 설계")를 실제 장치로 확인한다. 숨긴 창 스왑체인에
// 한 그래프로 장면(빨강 그래프 텍스처) → tone map(스왑체인 clear, 장면을 화면 가득) → Copy
// pass(초록 텍스처 복사) → overlay(스왑체인 load, 가운데 사각형에 복사본)를 그린다. overlay pass의
// CopyPassTarget 캡처가 가장자리 빨강(앞 pass 내용을 이어 받음)·가운데 초록이고, 프레임마다 장치
// present가 하나씩 늘며, 직렬·병렬 기록·즉시 실행 모드의 픽셀이 같고, present 뒤 load가 거부되며
// 검증 오류가 0인지 본다. 병렬 기록이 같은 샘플 렌더러를 worker 여럿에서 함께 쓰지 않도록 그리는
// pass마다 렌더러를 따로 만든다(렌더러는 그리기 통계를 고친다). device는 호출 스레드가 소유한다.
// 실패 수를 돌려준다.
rocks::uint32 RunSwapChainShareChecks(const char* name, rocks::graphics::Device& device,
                                      rocks::Allocator& allocator) noexcept;

} // namespace sandbox
