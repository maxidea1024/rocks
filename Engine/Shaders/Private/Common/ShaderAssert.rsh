// GPU assert 매크로다(Docs/Design/EngineShaders 4.1절 A01, 결정 SLD14·SLD18).
//
// ROCKS_SHADER_ASSERT(condition, code): condition이 거짓이면 진단 기록을 남긴다. SL0에서는 아무
// 것도 하지 않는 빈 매크로다(GPU 메시지 버퍼 Debug/GpuMessages.rsh는 SL2). SL2 뒤에도 VS·PS는
// ROCKS_FEATURE_DEBUG_PIXEL_UAV가 없는 형식(D3D11·GLES)에서 빈 매크로로 남는다. 인자는 평가하지
// 않으므로 부수 효과가 있는 식을 넣지 않는다. code는 호출자가 정한 uint 진단 번호다.
//
// 사용 예:
//   ROCKS_SHADER_ASSERT(count <= MaxLights, 1u);
#pragma once

#define ROCKS_SHADER_ASSERT(condition, code)
