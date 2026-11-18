#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 파이프라인 완성(RenderBase 4.4절 GX3)과 명령 기록 확장(GX4)을 실제 장치로 확인한다: compute가
// 구조화·typed UAV에 쓰고 VS·PS가 SRV로 읽어 그린 픽셀, compute가 쓴 간접 인자로 간접 그리기·
// dispatch, MRT 8 타깃별 clear·블렌드·쓰기 mask 값, 깊이·스텐실·블렌드·scissor·viewport 조합과 읽기
// 전용 깊이 attachment, 기본도형 종류, 버퍼·텍스처 복사와 UAV clear 바이트, attachment 없는 패스,
// 능력·규칙 위반의 명시 실패(InvalidArgument·Unsupported·IgnoredDraws), 검증 계층 오류 0. device는
// 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunCommandChecks(const char* name, rocks::graphics::Device& device,
                               rocks::Allocator& allocator) noexcept;

} // namespace sandbox
