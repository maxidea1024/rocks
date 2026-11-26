#pragma once

#include <Core/Result.h>
#include <RenderBase/ShaderResourceMap.h>
#include <RenderGraph/GraphPassAccesses.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 렌더 그래프 파라미터 수집기다(Docs/Design/ShaderSystem 16.2절 SSD65, RenderBase 8절 RG1 남은
// 것, UE FRDGBuilder::SetupPassResources의 파라미터 구조체 훑기 대응). 생성 파라미터 구조체의
// 그래프 멤버를 훑어 명시 선언 builder(GraphPassAccesses)와 같은 접근 목록을 만든다. 그래서
// culling·수명·전이·검증은 수집 경로와 무관하다(RenderGraph::AddPass의 구조체 판이 이 함수를
// 부른다).
namespace rocks::render {

// parameters(layout의 C++ 구조체 값)의 그래프 멤버를 오프셋 순으로 읽어 out에 넣는다.
//
// 멤버 → 접근(pass 종류 kind의 SRV는 Raster SrvGraphics, Compute·AsyncCompute SrvCompute):
// - GraphTextureParameter: Texture(텍스처 전체, SRV)
// - GraphTextureSrvParameter: TextureView(view, SRV)
// - GraphBufferSrvParameter: BufferView(view, SRV)
// - GraphTextureUavParameter·GraphBufferUavParameter: TextureView·BufferView(view, UavCompute) —
//   Compute·AsyncCompute만
// - GraphTextureAccessParameter·GraphBufferAccessParameter: Texture·Buffer(그 접근·범위·표시)
// - GraphRenderTargetParameter: ColorTarget(colors[0, colorCount))·DepthTarget(hasDepth면) —
//   Raster만
//
// 장치 멤버(DeviceTextureParameter·DeviceBufferParameter)는 의존이 아니다. null handle 멤버는 빼고,
// used(이 구조체 표로 만든 자원 지도, 없으면 null)가 있으면 셰이더가 쓰지 않는(자리 없는) 셰이더
// 멤버도 뺀다(RenderBase 9.6절 "쓰지 않는 멤버", UE ClearUnusedGraphResources).
//
// 실패(InvalidArgument — reason에 영어 정적 문구, out은 앞 멤버까지 채워졌을 수 있다): layout이
// 생성 표가 아님·parameters null, used가 이 layout의 지도가 아님, 셰이더가 읽는 그래프 멤버가 null,
// Copy pass의 셰이더 멤버, Raster pass의 UAV 멤버, Raster가 아닌 pass의 렌더 타깃, 색 타깃이
// MaxColorTargets 초과. out의 arena 할당 실패는 그 오류다. handle의 정당성(다른 그래프·view 종류와
// 접근의 조합)은 AddPass가 검사한다.
//
// 사용 예:
//   GraphPassAccesses accesses(graph);
//   if (!CollectGraphPassAccesses(accesses, GraphPassKind::Compute,
//                                 shader::ShaderParameterLayoutOf<BlurParameters>(), parameters,
//                                 &blurMap)) { ... }
ROCKS_RENDERGRAPH_API Result<void>
CollectGraphPassAccesses(GraphPassAccesses& out, GraphPassKind kind,
                         const shader::ShaderParameterLayout& layout, const void* parameters,
                         const ShaderResourceMap* used, const char** reason = nullptr) noexcept;

} // namespace rocks::render
