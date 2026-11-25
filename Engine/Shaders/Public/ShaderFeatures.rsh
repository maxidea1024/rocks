// 형식 기능 매크로다(Docs/Design/EngineShaders 3.2절·4.1절 A01, ShaderSystem 11.1절 SC10).
//
// 정의 조립(ShaderPreprocessDefines)이 형식 정의 features 어휘를 늘 0·1로 정의한다:
// - ROCKS_FEATURE_COMPUTE: compute 단계(모든 형식 1).
// - ROCKS_FEATURE_WAVE_OPERATIONS: SM 6 웨이브 연산(지금 D3D12_SM6만 1). 웨이브 경로는 CS 전용
//   Common/WaveOperations.rsh(SL1)에서만 쓴다(결정 SLD5).
// - ROCKS_FEATURE_VERTEX_VIEWPORT_LAYER_OUTPUT: VS layer·viewport 출력. 런타임 확인 기능이라 원문은
//   퍼뮤테이션 정의로만 가른다(SS 7.2절 7항).
// - ROCKS_FEATURE_TYPED_UAV_LOAD: 형식 확장 typed UAV load(지금 D3D12_SM6만 1).
// - ROCKS_FEATURE_DEBUG_PIXEL_UAV: 개발 전용 PS 진단 버퍼(D3D12_SM6·VULKAN_SM5·METAL_SM5, 결정
//   SLD18).
//
// 이 파일은 정의를 만들지 않고 빠진 것을 컴파일 오류로 막는다(Rocks는 정의되지 않은 기능 매크로를
// 0으로 보지 않는다 — UE Definitions.usf와 다른 점, 4.1절). 형식 차이는 이 매크로와 Platform.rsh
// 매크로로만 가른다(3.5절).
//
// 사용 예:
//   #if ROCKS_FEATURE_WAVE_OPERATIONS
//   ... 웨이브 경로 ...
//   #else
//   ... groupshared 경로 ...
//   #endif
#pragma once

#if !defined(ROCKS_FEATURE_COMPUTE) || !defined(ROCKS_FEATURE_WAVE_OPERATIONS) ||                  \
    !defined(ROCKS_FEATURE_VERTEX_VIEWPORT_LAYER_OUTPUT) || !defined(ROCKS_FEATURE_TYPED_UAV_LOAD) || \
    !defined(ROCKS_FEATURE_DEBUG_PIXEL_UAV)
#error "every ROCKS_FEATURE_* must be defined by the shader compiler (ShaderSystem SC10)"
#endif

#if (ROCKS_FEATURE_COMPUTE != 0 && ROCKS_FEATURE_COMPUTE != 1) ||                                  \
    (ROCKS_FEATURE_WAVE_OPERATIONS != 0 && ROCKS_FEATURE_WAVE_OPERATIONS != 1) ||                  \
    (ROCKS_FEATURE_TYPED_UAV_LOAD != 0 && ROCKS_FEATURE_TYPED_UAV_LOAD != 1) ||                    \
    (ROCKS_FEATURE_DEBUG_PIXEL_UAV != 0 && ROCKS_FEATURE_DEBUG_PIXEL_UAV != 1)
#error "ROCKS_FEATURE_* must be 0 or 1"
#endif
