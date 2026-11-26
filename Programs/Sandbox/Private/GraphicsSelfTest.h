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
// 장치마다 엔진 셰이더 probe만 돈다(Sandbox --engine-shader-self-test — 반복 검증용). scope가
// CookedShaders면 장치마다 쿠킹 엔진 셰이더 화면 검사만 돈다.
// self-test 범위다.
enum class GraphicsSelfTestScope : rocks::uint8 {
    // 모든 검사(장치마다 쿠킹 엔진 셰이더 화면 검사 포함 — 엔진 쿠킹 결과가 없으면 그 검사는 NOT
    // RUN).
    All,
    // 엔진 셰이더 probe만(Sandbox --engine-shader-self-test).
    EngineShaders,
    // 쿠킹 엔진 셰이더 라이브러리 화면 검사만(Sandbox --cooked-shader-self-test,
    // ShaderSystem 16.1절 SSD48 — 공급자가 없는 Release에서도 돈다).
    CookedShaders,
    // 쿠킹 텍스처 장면 대조만(Sandbox --texture-self-test, RenderBase 18.5절 7항 — 메모리 플랫폼
    // 데이터 장면과 시험 콘텐츠 쿠킹 장면).
    Textures
};

int RunGraphicsSelfTest(rocks::Allocator& allocator, rocks::ModuleLoader& modules,
                        bool gpuValidation,
                        GraphicsSelfTestScope scope = GraphicsSelfTestScope::All) noexcept;

} // namespace sandbox
