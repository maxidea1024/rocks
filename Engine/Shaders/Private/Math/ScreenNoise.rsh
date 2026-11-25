// 화면 공간 잡음이다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 텍스처는 인자로 받는다(자원 선언
// 없음 — 7.1절).
// - InterleavedGradientNoise: Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
//   Warfare", SIGGRAPH 2014 Advances in Real-Time Rendering(식 frac(52.9829189 · frac(dot(p, (0.06711056,
//   0.00583715))))). 프레임마다 화면 좌표를 5.588238 픽셀씩 옮겨 시간 축으로도 고르게 한다(같은
//   발표의 시간 변형). 실수 연산이라 형식 사이 결과는 허용 오차 안에서 같다(8.3절).
// - 파란 잡음 텍스처 좌표: 정수 연산(R2 수열 고정소수점 오프셋)이라 bit 단위로 같다.
#pragma once

#include "/Engine/Private/Math/LowDiscrepancy.rsh"
#include "/Engine/Private/Math/ScalarMath.rsh"

static const float2 InterleavedGradientWeights = float2(0.06711056, 0.00583715);
static const float InterleavedGradientScale = 52.9829189;
// 프레임당 화면 오프셋(픽셀)과 반복 주기(프레임)다.
static const float InterleavedGradientFrameOffset = 5.588238;
static const uint InterleavedGradientFramePeriod = 64u;

// 픽셀 위치(정수 픽셀 중심 좌표가 아니라 정수 좌표 — SV_Position.xy의 floor)의 IGN 값 [0, 1)다.
float InterleavedGradientNoise(float2 pixel) {
    return frac(InterleavedGradientScale * frac(dot(pixel, InterleavedGradientWeights)));
}

// 프레임 회전을 넣은 IGN이다(frame % 64 주기).
float InterleavedGradientNoise(float2 pixel, uint frame) {
    const float offset = InterleavedGradientFrameOffset * (float)(frame % InterleavedGradientFramePeriod);
    return InterleavedGradientNoise(pixel + offset);
}

// 한 변이 2^log2Size인 타일 파란 잡음 텍스처에서 이 픽셀·프레임이 읽을 texel이다. 프레임마다 R2
// 오프셋(LowDiscrepancy R2 증분)으로 타일을 옮겨 같은 잡음이 화면에 고정되지 않게 한다. log2Size는
// 1–16이다.
uint2 BlueNoiseTexel(uint2 pixel, uint frame, uint log2Size) {
    const uint2 offset = uint2(frame * R2SequenceStepX, frame * R2SequenceStepY) >> (32u - log2Size);
    const uint mask = (1u << log2Size) - 1u;
    return (pixel + offset) & mask;
}
