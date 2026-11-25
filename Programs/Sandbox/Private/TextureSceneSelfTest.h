#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace rocks {

class ModuleLoader;

} // namespace rocks

// 쿠킹 텍스처 장면 대조다(Docs/Design/RenderBase 18.5절 7항 — SM7, 18.1절 완료 기준 3항).
namespace sandbox {

// 메모리 플랫폼 데이터 장면을 실제 장치로 확인한다(18.5절 7항 (가)): 다섯 차원 × 대표 형식(LDR·
// sRGB·HDR 16F·32F·단일 채널 .rrrr)과 정수 형식(2D·2D 배열·volume × R8Uint·RGBA16Sint)의 텍스처를
// TexturePlatformData → RenderBase CookedTexture 경로로 올려 텍스처 표시로 그리고, 표본점 픽셀 =
// CPU 기준(byte 해석 → 쿠킹 swizzle → 표시 방식 → sRGB 부호화, 채널 ±2)을 단언한다. 음성: 잘린
// bulk· mip byte 수 불일치·장치 한도 초과·장치가 못 하는 형식(검사 오류와 원인), 장치 생성 실패의
// ResourceFailed와 typed 대체 표시. Thread·Inline 두 모드 픽셀 동일, 처음 돈 장치와의 표본점 차
// ±8(backend 사이), 검증 오류 0, lane 작업·handle·장치 회수 0. 장치가 그 형식·차원을 못 하면 그
// 장면만 NOT RUN 사유를 남긴다. 실패 수를 돌려준다(진단은 stdout).
rocks::uint32 RunTextureSceneChecks(const char* name, rocks::graphics::Device& device,
                                    rocks::Allocator& allocator) noexcept;

// 시험 콘텐츠 쿠킹(Programs/Sandbox/TestContent — `asset-cook --profile=<host>`)의 텍스처를 엔진
// (EngineAssets — 쿠킹 루트 지정)으로 요청해 패키지 공개 뒤 올리고, 같은 CPU 기준(쿠킹 byte에서
// 계산)으로 대조한다(18.5절 7항 (나)). 쿠킹이 없으면 NOT RUN이다. 실패 수를 돌려준다.
rocks::uint32 RunCookedTextureChecks(const char* name, rocks::graphics::Device& device,
                                     rocks::Allocator& allocator,
                                     rocks::ModuleLoader& modules) noexcept;

} // namespace sandbox
