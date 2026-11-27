#pragma once

#include "EngineShaderProbe.h"

#include <Graphics/Device.h>
#include <ShaderCore/ShaderParameterTypes.h>

// 엔진 셰이더 SL1 probe의 장치 연결이다(Docs/Design/EngineShaders 14.6절 2항): CS 전용
// API(ComputeDispatch· WaveOperations) compute probe와 텍스처를 읽는 유틸리티 PS probe.
// EngineShaderSelfTest.cpp가 SL0 probe 뒤에 부른다.
namespace sandbox::engine_shaders {

// ProbeCommon.rsh ProbeConstants의 C++ 판이다(96 byte — 레이아웃 표는 SandboxShaders.h
// ProbeConstantLayouts).
struct alignas(16) ProbeConstants {
    rocks::shader::ShaderUint4 control;
    rocks::shader::ShaderFloat4 parameters;
    rocks::shader::ShaderFloat4x4 values;
};

// ComputeProbes.rsf CS probe(ComputeDispatch 함수·감싼 dispatch·간접 인자·그룹 연산 두 경로)를
// 돌리고 CPU 기준과 대조한다.
void RunComputeProbes(ProbeContext& context, rocks::graphics::Device& device) noexcept;

// UtilityProbes.rsf PS probe(Catmull–Rom 5 tap·정수 bilinear·texel 중심·LUT 표본)를 돌리고
// 대조한다.
void RunUtilityProbes(ProbeContext& context, rocks::graphics::Device& device) noexcept;

// A03 SL1 PS probe(LUT 좌표·SDR 부호화·색 지도·구분 색 — EngineShaderReferenceColorUtility.cpp)다.
void RunColorUtilityProbes(ProbeContext& context) noexcept;

} // namespace sandbox::engine_shaders
