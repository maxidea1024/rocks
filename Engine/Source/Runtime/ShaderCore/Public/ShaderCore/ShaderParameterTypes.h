#pragma once

#include <Base/Types.h>

// 셰이더 파라미터 구조체의 C++ 정렬 타입이다(Docs/Design/ShaderSystem 7.2절 5항, 12.2.1절 SSD27).
//
// Core Matrix4·Vector3는 정렬 지정이 없어 셰이더 상수 버퍼 오프셋과 맞지 않을 수 있다. 셰이더
// 파라미터 구조체(루트 상수·상수 버퍼의 C++ 쪽)는 이 타입만 쓴다: 2성분 alignas(8), 3·4성분과
// float4x4 alignas(16). 3성분은 크기 12 byte가 아니라 정렬 때문에 16 byte다 — 3성분 뒤에 스칼라를
// 채우는 HLSL 배치(float3 a; float b; → b가 12)는 C++에서 같은 구조체로 쓸 수 없으므로 그런 자리는
// ShaderFloat4로 쓰고 레이아웃 표에 두 멤버로 적는다. 행렬은 행 우선 저장(UE와 같다, 7.2절
// 6항)이다.
//
// 사용 예:
//   struct alignas(16) BlitConstants {
//       ShaderFloat4x4 transform;   // offset 0
//       ShaderFloat2 uvScale;       // offset 64
//       ShaderFloat2 uvBias;        // offset 72
//   };
//   static_assert(offsetof(BlitConstants, uvBias) == 72);
namespace rocks::shader {

struct alignas(8) ShaderFloat2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct alignas(16) ShaderFloat3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct alignas(16) ShaderFloat4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct alignas(8) ShaderInt2 {
    int32 x = 0;
    int32 y = 0;
};

struct alignas(16) ShaderInt3 {
    int32 x = 0;
    int32 y = 0;
    int32 z = 0;
};

struct alignas(16) ShaderInt4 {
    int32 x = 0;
    int32 y = 0;
    int32 z = 0;
    int32 w = 0;
};

struct alignas(8) ShaderUint2 {
    uint32 x = 0;
    uint32 y = 0;
};

struct alignas(16) ShaderUint3 {
    uint32 x = 0;
    uint32 y = 0;
    uint32 z = 0;
};

struct alignas(16) ShaderUint4 {
    uint32 x = 0;
    uint32 y = 0;
    uint32 z = 0;
    uint32 w = 0;
};

// 4×4 행렬(행 우선 — m[행][열])이다.
struct alignas(16) ShaderFloat4x4 {
    float m[4][4] = {};
};

static_assert(sizeof(ShaderFloat2) == 8 && alignof(ShaderFloat2) == 8);
static_assert(sizeof(ShaderFloat3) == 16 && alignof(ShaderFloat3) == 16);
static_assert(sizeof(ShaderFloat4) == 16 && alignof(ShaderFloat4) == 16);
static_assert(sizeof(ShaderInt2) == 8 && sizeof(ShaderInt3) == 16 && sizeof(ShaderInt4) == 16);
static_assert(sizeof(ShaderUint2) == 8 && sizeof(ShaderUint3) == 16 && sizeof(ShaderUint4) == 16);
static_assert(sizeof(ShaderFloat4x4) == 64 && alignof(ShaderFloat4x4) == 16);

} // namespace rocks::shader
