#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 텍스처 차원·view swizzle·샘플러 확장(AssetSystem A0-G)을 실제 장치로 확인한다.
// - 차원: 2D 배열·cube·cube 배열·3D를 mip 2개(3D는 3개)로 만들어 모든 mip을 ReadTexture로 읽은
//   byte가 올린 byte와 같고, UpdateTexture 왕복도 같은지. 셰이더로 layer·깊이 면을 load하고 cube는
//   면마다 두 방향(UE·D3D 면 순서와 면 안 u·v 방향)을 표본한 색이 기대값인지. slice 범위 전이
//   뒤에도 그린 값이 같은지.
// - swizzle: TextureDesc·TextureViewDesc의 성분 대응을 셰이더로 읽은 값. 같은 텍스처의 원본과
//   view를 번갈아 그려도 각자의 대응이다(OpenGL ES는 텍스처 객체 상태를 공유한다). A8 형식 흉내와의
//   합성. D3D11은 항등이 아니면 Unsupported다.
// - 샘플러: 3D W축 주소 모드(Wrap·Clamp), 깊이 비교 샘플러(LessEqual·GreaterEqual), 필터 5종의 생성
//   (AnisotropicPoint는 하드웨어 대응이 없으면 AnisotropicLinear로 만들어진다). 그 밖에 staging
//   업로드·BC1 왕복, swizzle이 attachment·ReadTexture에 새지 않음, 비교 샘플러 선형 필터.
// device는 호출 스레드가 소유한다. 실패 수를 돌려준다. 읽기를 지원하지 않는 조합(OpenGL ES의 2D가
// 아닌 깊이 텍스처 ReadTexture 등)은 Unsupported인지 단언한다.
rocks::uint32 RunTextureChecks(const char* name, rocks::graphics::Device& device,
                               rocks::Allocator& allocator) noexcept;

} // namespace sandbox
