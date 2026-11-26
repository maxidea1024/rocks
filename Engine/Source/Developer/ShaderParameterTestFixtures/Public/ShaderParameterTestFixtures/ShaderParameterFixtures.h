#pragma once

#include <Base/Types.h>
#include <ShaderCore/ShaderParameterTypes.h>

// 셰이더 파라미터 구조체 시험 fixture다(Docs/Design/ShaderSystem 16.2절 SSD58). 빌드가 이 헤더에서
// ShaderParameterFixtures.shaderparams.h(생성 표)와 /Engine/Generated/ShaderParameterTestFixtures/
// <이름>.rsh(생성 HLSL)를 만든다. 주석의 오프셋은 C++ 오프셋이고 생성 표·워커 리플렉션과 같아야 한다.
namespace rocks::shader_test_fixtures {

// 중첩용 구조체다. direction(3성분) 다음 intensity는 C++에서 16이므로 생성 HLSL이 12에 padding을 넣는다.
ROCKS_SHADER_PARAMETERS()
struct alignas(16) FixtureLight {
    shader::ShaderFloat3 direction; // 0
    float intensity;                // 16 (padding 12)
    shader::ShaderFloat4 color;     // 32
};

// 상수 버퍼(cbuffer FixtureScene) 정렬 경계 모음이다.
ROCKS_SHADER_PARAMETERS()
struct alignas(16) FixtureScene {
    shader::ShaderFloat4x4 worldToClip; // 0
    shader::ShaderFloat3 tint;          // 64 (padding 76)
    float exposure;                     // 80
    shader::ShaderFloat2 uvScale;       // 88
    shader::ShaderInt3 offsets;         // 96 (int padding 108)
    int32 layer;                        // 112
    shader::ShaderUint4 counts;         // 128
    shader::ShaderFloat4 weights[3];    // 144
    FixtureLight light;                 // 192
    float tail;                         // 240
};

// 루트 상수(그리기별 상수 — 셰이더 이름 FixtureRoot)다.
ROCKS_SHADER_PARAMETERS(Name = "FixtureRoot", RootConstants)
struct alignas(16) FixtureDraw {
    shader::ShaderFloat4x4 transform; // 0
    shader::ShaderFloat3 color;       // 64 (padding 76)
    float alpha;                      // 80
    shader::ShaderUint4 ids;          // 96
};

} // namespace rocks::shader_test_fixtures
