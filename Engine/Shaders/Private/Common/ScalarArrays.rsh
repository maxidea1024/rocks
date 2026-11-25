// float4 배열로 묶은 스칼라 배열의 원소 읽기다(Docs/Design/EngineShaders 4.1절 A01, ShaderSystem
// 7.2절 5항 배열 규칙 — 상수 버퍼의 스칼라·2성분 배열은 원소 크기가 16 byte 배수가 아니라 금지이므로
// float4 배열 하나에 넷씩 담는다. UE SHADER_PARAMETER_SCALAR_ARRAY·GET_SCALAR_ARRAY_ELEMENT와 같은
// 배치다).
//
// 스칼라 i는 packed[i / 4]의 (i % 4)번째 성분이다. 상수 버퍼 멤버에도 함수 인자로 받은 배열에도
// 쓸 수 있게 매크로로 둔다(HLSL 2018에는 배열 길이 템플릿이 없다). 범위 검사는 하지 않는다(i는
// 4 × 배열 길이보다 작아야 한다).
//
// 사용 예(C++ float weights[8] → float4 Weights[2]):
//   float w = ROCKS_SCALAR_ARRAY_ELEMENT(Constants.Weights, index);
//   uint id = ROCKS_SCALAR_ARRAY_ELEMENT(Constants.Ids, index);   // uint4 배열도 같다
#pragma once

// 묶음 하나의 스칼라 수다.
static const uint ScalarArrayComponents = 4u;

#define ROCKS_SCALAR_ARRAY_ELEMENT(packed, index)                                                  \
    ((packed)[(uint)(index) / ScalarArrayComponents][(uint)(index) % ScalarArrayComponents])

// 스칼라 n개를 담는 float4 원소 수다(C++ 쪽 배열 크기 계산과 같은 식).
uint ScalarArrayPackedLength(uint scalarCount) {
    return (scalarCount + ScalarArrayComponents - 1u) / ScalarArrayComponents;
}
