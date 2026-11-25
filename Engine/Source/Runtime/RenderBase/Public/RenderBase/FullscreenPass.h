#pragma once

#include <Core/Result.h>
#include <Graphics/Device.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/ShaderSet.h>

// 전체화면 pass 도우미다(Docs/Design/RenderBase 11.2절 "전체화면 pass", 11.4절 3항, UE
// FPixelShaderUtils 대응). VS는 엔진 셰이더 FullscreenTriangleVertex(정점 번호로 대상을 덮는 삼각형
// 하나)이고 정점 버퍼·index 버퍼가 없다. UE는 화면 사각형 정점·index 버퍼를 쓰지만 Rocks는 속성
// 없는 파이프라인과 버퍼 없는 그리기를 기본으로 한다. 렌더 그래프 쪽은
// RenderGraph::AddFullscreenPass다.
//
// 사용 예(텍스처를 대상 전체에 복사):
//   auto copy = set.FindEntry(render::CopyTexturePixelType.hash);
//   auto request = render::FullscreenPipelineRequest(set, copy.Value(), PixelFormat::RGBA8Unorm);
//   request.Value().state.bindings.texture = render::CopyTextureSourceName;
//   auto pipeline = cache.FindOrCreate(request.Value());
//   commands.SetPipeline(pipeline.Value());
//   commands.SetTexture(source);
//   render::DrawFullscreenTriangle(commands);
namespace rocks::render {

// 전체화면 삼각형의 정점 수다.
inline constexpr uint32 FullscreenTriangleVertexCount = 3;

// set의 FullscreenTriangleVertex와 pixelEntry로 그리는 파이프라인 요청을 만든다. 정점 배치 없음,
// 색 타깃 하나(colorFormat), 불투명·컬 없음·깊이 끔(PipelineStatePresets)이고 쓰는 쪽이 상태를 더
// 바꿀 수 있다(블렌드·깊이·이름 표·그리기별 상수 크기). 실패: set에 FullscreenTriangleVertex가 없음
// NotFound, set이 Ready가 아님 NotReady(ShaderSet::FindEntry), pixelEntry가 범위 밖
// InvalidArgument.
ROCKS_RENDERBASE_API Result<GraphicsPipelineRequest>
FullscreenPipelineRequest(ShaderSet& set, uint32 pixelEntry,
                          graphics::PixelFormat colorFormat) noexcept;

// 전체화면 삼각형을 그린다(파이프라인·자원은 먼저 묶는다).
inline void DrawFullscreenTriangle(graphics::CommandContext& commands) noexcept {
    commands.Draw(FullscreenTriangleVertexCount);
}

} // namespace rocks::render
