#pragma once

#include <Core/Containers/Span.h>
#include <ShaderCore/ShaderType.h>

// RenderBase가 등록하는 엔진 셰이더 타입이다(Docs/Design/RenderBase 11.4절 2항). UE도 화면 VS·
// 복사·clear 같은 유틸리티 셰이더를 RenderCore가 등록한다. 원문은 Engine/Shaders(/Engine)에 있다.
//
// 공개 헤더에는 이름·해시 상수만 둔다(desc는 정의 모듈의 cpp — ShaderSystem 12.2.1절 SSD2). 엔진
// 호스트는 RenderBase 모듈 수명 객체가 호스트 등록부(ShaderTypeRegistry 서비스)에 넣는다. 등록부를
// 직접 만드는 호스트(Sandbox·시험)는 EngineShaderTypes()를 자기 목록과 함께 등록한다.
//
// 사용 예(전체화면 복사 — RenderBase/FullscreenPass.h):
//   auto copy = set.FindEntry(render::CopyTexturePixelType.hash);
//   auto request = render::FullscreenPipelineRequest(set, copy.Value(), PixelFormat::RGBA8Unorm);
namespace rocks::render {

// 전체화면 삼각형 VS다(/Engine/Private/Utility/FullscreenTriangle.rsf). 출력은 SV_Position과
// TEXCOORD0(화면 UV — 왼쪽 위 (0,0))이다. 정점 버퍼 없이 Draw(3)으로 그린다.
ROCKS_SHADER_TYPE_NAME(FullscreenTriangleVertexType, "FullscreenTriangleVertex");

// 2D 텍스처 복사 PS다(/Engine/Private/Utility/CopyTexture.rsf). 텍스처 slot 이름은
// CopyTextureSourceName이고 FullscreenTriangleVertex와 짝이다.
ROCKS_SHADER_TYPE_NAME(CopyTexturePixelType, "CopyTexturePixel");

// CopyTexturePixel의 텍스처 이름이다(PipelineBindingNames::texture).
inline constexpr char CopyTextureSourceName[] = "Source";

// RenderBase가 등록하는 엔진 셰이더 타입 목록이다(정적 수명).
ROCKS_RENDERBASE_API Span<const shader::ShaderTypeDesc* const> EngineShaderTypes() noexcept;

} // namespace rocks::render
