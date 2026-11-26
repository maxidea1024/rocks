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
// 셰이더 파라미터 구조체 표지다(Docs/Design/ShaderSystem 16.2절 SSD50). 바로 뒤에 이름공간 범위의
// 비템플릿 struct 정의가 와야 한다. 빌드가 줄 첫머리의 이 표지로 헤더를 찾아 MetaCompiler 셰이더
// 파라미터 모드를 돌리고, <헤더>.shaderparams.h(ShaderParameterStruct<T> — 레이아웃
// 표·identity·생성 HLSL)와 /Engine/Generated/<모듈>/<C++ 이름>.rsh(생성 HLSL)를 만든다. 비정적
// 데이터 멤버 전부가 파라미터이고 타입은 float·int32·uint32, 이 헤더의 정렬 타입, 같은 모듈의 다른
// 파라미터 구조체(중첩 1단), 그 1차원 배열(원소 16 byte 배수)만 받는다. 크기는 16 byte 배수다.
//
// 지정자: Name = "…"(HLSL cbuffer·루트 상수 변수 이름 = 레이아웃 표 이름, 없으면 C++ 이름),
// RootConstants(그리기별 상수 — 128 byte 이하, 중첩 구조체 없음).
//
// 사용 예:
//   ROCKS_SHADER_PARAMETERS(Name = "Tonemap", RootConstants)
//   struct alignas(16) TonemapConstants {
//       shader::ShaderFloat4 exposure;
//       shader::ShaderFloat4 film;
//   };
//   // HLSL: #include "/Engine/Generated/RenderBase/TonemapConstants.rsh" → Tonemap.exposure
#define ROCKS_SHADER_PARAMETERS_JOIN_INNER(left, right) left##right
#define ROCKS_SHADER_PARAMETERS_JOIN(left, right) ROCKS_SHADER_PARAMETERS_JOIN_INNER(left, right)
#define ROCKS_SHADER_PARAMETERS(...)                                                               \
    enum {                                                                                         \
        ROCKS_SHADER_PARAMETERS_JOIN(rocksShaderParametersMarker_, __COUNTER__)                    \
        [[clang::annotate("rocks", "rocks.shaderparams.marker", #__VA_ARGS__)]] = 0                \
    };

namespace rocks::shader {

struct alignas(8) ShaderFloat2 {
    float x = 0.0f;
    float y = 0.0f;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr float& operator[](usize index) noexcept {
        return index == 0 ? x : y;
    }

    constexpr const float& operator[](usize index) const noexcept {
        return index == 0 ? x : y;
    }
};

struct alignas(16) ShaderFloat3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr float& operator[](usize index) noexcept {
        return index == 0 ? x : (index == 1 ? y : z);
    }

    constexpr const float& operator[](usize index) const noexcept {
        return index == 0 ? x : (index == 1 ? y : z);
    }
};

struct alignas(16) ShaderFloat4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr float& operator[](usize index) noexcept {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }

    constexpr const float& operator[](usize index) const noexcept {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }
};

struct alignas(8) ShaderInt2 {
    int32 x = 0;
    int32 y = 0;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr int32& operator[](usize index) noexcept {
        return index == 0 ? x : y;
    }

    constexpr const int32& operator[](usize index) const noexcept {
        return index == 0 ? x : y;
    }
};

struct alignas(16) ShaderInt3 {
    int32 x = 0;
    int32 y = 0;
    int32 z = 0;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr int32& operator[](usize index) noexcept {
        return index == 0 ? x : (index == 1 ? y : z);
    }

    constexpr const int32& operator[](usize index) const noexcept {
        return index == 0 ? x : (index == 1 ? y : z);
    }
};

struct alignas(16) ShaderInt4 {
    int32 x = 0;
    int32 y = 0;
    int32 z = 0;
    int32 w = 0;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr int32& operator[](usize index) noexcept {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }

    constexpr const int32& operator[](usize index) const noexcept {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }
};

struct alignas(8) ShaderUint2 {
    uint32 x = 0;
    uint32 y = 0;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr uint32& operator[](usize index) noexcept {
        return index == 0 ? x : y;
    }

    constexpr const uint32& operator[](usize index) const noexcept {
        return index == 0 ? x : y;
    }
};

struct alignas(16) ShaderUint3 {
    uint32 x = 0;
    uint32 y = 0;
    uint32 z = 0;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr uint32& operator[](usize index) noexcept {
        return index == 0 ? x : (index == 1 ? y : z);
    }

    constexpr const uint32& operator[](usize index) const noexcept {
        return index == 0 ? x : (index == 1 ? y : z);
    }
};

struct alignas(16) ShaderUint4 {
    uint32 x = 0;
    uint32 y = 0;
    uint32 z = 0;
    uint32 w = 0;

    // 성분 index(0부터, 범위 밖이면 마지막 성분)다.
    constexpr uint32& operator[](usize index) noexcept {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }

    constexpr const uint32& operator[](usize index) const noexcept {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }
};

// 4×4 행렬(행 우선 — m[행][열])이다.
struct alignas(16) ShaderFloat4x4 {
    float m[4][4] = {};

    // 행 row(0–3)다(m[row]).
    constexpr float (&operator[](usize row) noexcept)[4] {
        return m[row];
    }

    constexpr const float (&operator[](usize row) const noexcept)[4] {
        return m[row];
    }
};

static_assert(sizeof(ShaderFloat2) == 8 && alignof(ShaderFloat2) == 8);
static_assert(sizeof(ShaderFloat3) == 16 && alignof(ShaderFloat3) == 16);
static_assert(sizeof(ShaderFloat4) == 16 && alignof(ShaderFloat4) == 16);
static_assert(sizeof(ShaderInt2) == 8 && sizeof(ShaderInt3) == 16 && sizeof(ShaderInt4) == 16);
static_assert(sizeof(ShaderUint2) == 8 && sizeof(ShaderUint3) == 16 && sizeof(ShaderUint4) == 16);
static_assert(sizeof(ShaderFloat4x4) == 64 && alignof(ShaderFloat4x4) == 16);

} // namespace rocks::shader
