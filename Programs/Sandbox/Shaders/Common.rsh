// Sandbox self-test 공용 셰이더 도우미다(Docs/Design/ShaderSystem 16.1절 SSD19).
#pragma once

// 정점 번호(0, 1, 2)로 대상을 덮는 삼각형의 clip 공간 위치다.
float4 FullScreenPosition(uint id) {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 0.5, 1.0);
}
