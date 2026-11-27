#pragma once

#include <Core/Result.h>
#include <Graphics/Device.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/ShaderSet.h>

// 런타임 밉 생성 도우미다(Docs/Design/EngineShaders 14.4절 5항·14.5절 2항, 결정 SLD23, UE
// FGenerateMips 대응). 텍스처의 mip 0(또는 지금 내용)에서 mip 1부터 끝까지를 만든다. 쿠킹 텍스처는
// mip을 갖고 오므로 런타임에 만든 텍스처(렌더 결과·절차 생성)가 쓴다.
//
// 두 경로를 장치 능력으로 고른다(UE와 같다):
// - Compute: Storage 용도이고 형식에 StorageWrite·storage image 대응(UtilityShaderTypes.h
//   StorageImageFormatOf — float 계열)이 있는 2D·2D 배열·3D. 기록 하나에 mip마다 dispatch.
// - Raster: RenderTarget 용도·능력이 있는 2D·2D 배열·cube·3D(sRGB 형식은 이 경로 — 필터는 선형
// 공간).
//   (mip, slice)마다 pass 하나.
// 필터는 이전 mip에서 정확한 면적 박스(홀수 크기 포함)다.
//
// 사용 예(렌더 lane에서, 프레임을 기록하지 않을 때):
//   auto generated = render::GenerateMips(device, engineShaders, pipelines, texture, desc);
//   if (!generated) { ... }
namespace rocks::render {

// 밉 생성 경로다.
enum class MipGenerationPath : uint8 {
    Compute,
    Raster
};

// desc 텍스처의 밉 생성 경로를 고른다(Compute 조건을 먼저 본다). 실패: mip이 하나뿐이거나 MSAA
// InvalidArgument, 두 경로 모두 안 됨(용도·형식 능력·cube 배열) Unsupported.
ROCKS_RENDERBASE_API Result<MipGenerationPath>
SelectMipGenerationPath(const graphics::Device& device, const graphics::TextureDesc& desc) noexcept;

// path 경로가 desc 텍스처에 쓸 수 있는지다(SelectMipGenerationPath와 같은 규칙, 시험·경로 강제용).
ROCKS_RENDERBASE_API bool CanGenerateMips(const graphics::Device& device,
                                          const graphics::TextureDesc& desc,
                                          MipGenerationPath path) noexcept;

// texture(desc로 만든 텍스처 — view가 아니다)의 mip 1 – mipLevels − 1을 path 경로로 만든다. 장치가
// 프레임을 기록하지 않을 때 소유 스레드에서 부른다(아니면 BeginPass의 Busy). 만든 view는 함수
// 안에서 Retire한다. 실패: path를 쓸 수 없음 Unsupported(CanGenerateMips), mip 하나
// InvalidArgument, 셰이더 집합에 그 퍼뮤테이션이 없음 NotFound, 파이프라인·view·pass 생성 실패는 그
// 오류(앞 mip은 이미 만들어졌을 수 있다).
ROCKS_RENDERBASE_API Result<void> GenerateMips(graphics::Device& device, ShaderSet& set,
                                               PipelineCache& cache,
                                               graphics::TextureHandle texture,
                                               const graphics::TextureDesc& desc,
                                               MipGenerationPath path) noexcept;

// SelectMipGenerationPath가 고른 경로로 만든다.
ROCKS_RENDERBASE_API Result<void> GenerateMips(graphics::Device& device, ShaderSet& set,
                                               PipelineCache& cache,
                                               graphics::TextureHandle texture,
                                               const graphics::TextureDesc& desc) noexcept;

} // namespace rocks::render
