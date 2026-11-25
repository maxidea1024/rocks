// 엔진 기본 정점 입력 타입 BuiltinVertexInput이다(Docs/Design/RenderBase 10.6절 3항, 엔진 셰이더
// A05). 내장 메시와 fallback 메시 셰이더가 쓴다. 정점 입력 축 셰이더는 이 파일을 직접 include하지
// 않고 /Engine/Generated/VertexInput.rsh를 include한다(셰이더 컴파일러가 이 파일로 잇는다).
//
// 배치(RenderBase BuiltinVertexLayout): stream 0 하나, stride 36 byte — 위치 float3(ATTRIBUTE0,
// 0)·UV float2(ATTRIBUTE1, 12)·색 float4(ATTRIBUTE2, 20).
#pragma once

struct VertexInputAttributes {
    float3 position : ATTRIBUTE0;
    float2 texCoord : ATTRIBUTE1;
    float4 color : ATTRIBUTE2;
};

// local 공간 위치(cm)다.
float3 GetVertexInputLocalPosition(VertexInputAttributes input) {
    return input.position;
}

// 텍스처 좌표(UV 채널 0)다.
float2 GetVertexInputTexCoord(VertexInputAttributes input) {
    return input.texCoord;
}

// 정점 색(선형)이다.
float4 GetVertexInputColor(VertexInputAttributes input) {
    return input.color;
}
