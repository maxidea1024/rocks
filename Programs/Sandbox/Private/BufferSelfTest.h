#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 장치 능력(RenderBase 4.4절 GX1)을 로그로 남기고 일관성을 본다: 한도가 사양 최소 보장 이상인지,
// 형식마다 Multisample 능력 ⇔ 2표본 이상, 표본 수 집합 ⇔ 렌더 타깃·깊이 능력. 실패 수를 돌려준다.
rocks::uint32 RunCapsChecks(const char* name, rocks::graphics::Device& device) noexcept;

// 버퍼 용도·view(GX2)를 실제 장치로 확인한다: 용도별 생성·업로드·staging 복사·ReadBuffer 바이트
// 대조, 형식마다 typed view 생성 ⇔ TexelBufferRead·Write 능력, view·생성 음성 대조(형식·범위·용도·
// stale), VS·PS가 typed·구조화·raw view를 읽어 그린 픽셀 대조, view 배치 불일치 그리기 무시, 버퍼를
// view보다 먼저 놓은 경우, 검증 계층 오류 0. device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunBufferChecks(const char* name, rocks::graphics::Device& device,
                              rocks::Allocator& allocator) noexcept;

} // namespace sandbox
