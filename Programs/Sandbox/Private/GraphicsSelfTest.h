#pragma once

#include <Core/Memory/Allocator.h>

namespace rocks {

class ModuleLoader;

} // namespace rocks

namespace sandbox {

// 창 없이 그래픽 backend를 검사한다. D3D12·D3D11(각각 하드웨어·WARP)과 OpenGL에서 offscreen 렌더
// 타깃에 내장 삼각형·텍스처 사각형을 그리고 readback 픽셀(꼭짓점 색·UV 방향), 두 backend의 결과
// 일치, 검증 계층 오류 0을 확인한다. 사용할 수 없는 backend는 NOT RUN으로 보고한다. 실패가 하나라도
// 있으면 1을 반환한다. gpuValidation이 참이면 D3D12 장치를 GPU 기반 검증으로 만든다(아주 느리다,
// placed 자원·상태 추적 진단용, Sandbox --self-test --gpu-validation). 장치마다 엔진 셰이더 SL0 GPU
// probe(EngineShaderSelfTest)도 돌리고 끝에 장치 사이 대조를 찍는다. engineShadersOnly가 참이면
// 장치마다 엔진 셰이더 probe만 돈다(Sandbox --engine-shader-self-test — 반복 검증용).
int RunGraphicsSelfTest(rocks::Allocator& allocator, rocks::ModuleLoader& modules,
                        bool gpuValidation, bool engineShadersOnly = false) noexcept;

} // namespace sandbox
