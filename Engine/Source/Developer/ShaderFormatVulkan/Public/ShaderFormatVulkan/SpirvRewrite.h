#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Graphics/ShaderSourceCompiler.h>
#include <ShaderCompilerCommon/SpirvRewrite.h>

namespace rocks::shader {

// 그리기별 상수 블록의 이름이다(Graphics PipelineDesc::drawConstantBytes, ShaderSystem 7.2절 2항
// ROCKS_ROOT_CONSTANTS의 SH2 전 대역). 원문의 cbuffer DrawConstants가 push constant가 된다.
inline constexpr char DrawConstantsBlockName[] = "DrawConstants";

// DXC가 낸 SPIR-V(-fspv-reflect로 시맨틱 정보 포함)를 Rocks Vulkan 규약으로 다시 쓴다
// (Docs/Design/VulkanGraphics.ko.md 13절, ShaderSystem 7.2절 2·4·7항).
// - 정점 셰이더 입력 location을 시맨틱 ATTRIBUTEn(대소문자 무시)의 n으로 바꾼다. 시스템 값이 아닌
//   입력에 ATTRIBUTEn 시맨틱이 없거나, n이 15를 넘거나 겹치면 InvalidArgument다.
// - rootConstantsName 이름의 상수 버퍼(Uniform Block 변수)를 push constant로 바꾼다(변수·포인터
//   타입·access chain의 storage class, binding 장식 제거).
// - 나머지 descriptor 자원을 set = 단계(정점 0, 픽셀 1, compute 0), binding = 종류 순서(상수 버퍼 →
// 구조화·raw
//   버퍼 → storage image → SRV 텍스처 → 샘플러, 같은 종류는 원래 set·binding 순)로 다시 매긴다.
// - SPV_GOOGLE_hlsl_functionality1·SPV_GOOGLE_user_type 확장 선언과 그 장식을 지운다.
// 진입점의 실행 모델이 stage와 다르거나 SPIR-V가 망가졌으면 InvalidArgument다. 실패하면
// diagnostics에 영어 설명을 쓰고 output은 정해지지 않는다.
//
// 사용 예:
//   Array<uint32> rewritten(allocator);
//   auto done = shader::RewriteSpirv(dxcWords, graphics::ShaderStage::Vertex,
//                                    StringView(shader::DrawConstantsBlockName), rewritten,
//                                    diagnostics);
Result<void> RewriteSpirv(Span<const uint32> input, graphics::ShaderStage stage,
                          StringView rootConstantsName, Array<uint32>& output,
                          String& diagnostics) noexcept;

} // namespace rocks::shader
