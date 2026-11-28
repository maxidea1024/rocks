#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Hash.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderCore/ShaderPermutation.h>

// 셰이더 타입 기술이다(Docs/Design/ShaderSystem 12.2.1절 SSD2, UE
// FShaderType·IMPLEMENT_GLOBAL_SHADER 대응).
//
// 셰이더 타입 하나는 엔트리 원문 하나·단계 하나·퍼뮤테이션 도메인 하나다. 식별은 이름의 64-bit
// 해시(nameHash — CP7, Core HashStringBytesIgnoringAsciiCase)이고, 라이브러리 항목·조회·TypedShader
// 조회가 모두 해시로 찾는다(이름은 진단용).
//
// 정의 자리(Modular 링크 — BuildTargets BD29): ShaderTypeDesc 값은 **정의 모듈의 cpp**에 둔다. 공개
// 헤더에는 ROCKS_SHADER_TYPE_NAME으로 이름·해시 상수만 둔다. 다른 모듈은 해시로 등록부
// (ShaderTypeRegistry)에서 desc를 찾는다.
//
// 사용 예:
//   // 공개 헤더(다른 모듈이 해시로 찾는다):
//   ROCKS_SHADER_TYPE_NAME(FullscreenTriangleVS, "FullscreenTriangleVS");
//   // 정의 모듈의 cpp:
//   constexpr ShaderTypeDesc FullscreenTriangleVSDesc = MakeShaderTypeDesc(
//       FullscreenTriangleVS, "/Engine/Private/FullscreenTriangle.rsf", "MainVS",
//       ShaderStageVertex, ShaderTypeOwner::Engine);
//   constexpr const ShaderTypeDesc* ModuleShaderTypes[] = {&FullscreenTriangleVSDesc};
//   // 모듈 수명 객체가 ShaderTypeModuleRegistration(ModuleShaderTypes)으로 등록한다.
namespace rocks::shader {

// 셰이더 타입 이름의 최대 byte 수(NUL 제외)다.
inline constexpr usize MaxShaderTypeNameBytes = 64;

// 셰이더 타입 이름의 해시다(CP7 — 대소문자를 가리지 않는다. 이름만 대소문자가 다른 두 타입은 같은
// 해시라 등록에서 충돌한다).
constexpr uint64 ShaderTypeNameHash(StringView name) noexcept {
    return HashStringBytesIgnoringAsciiCase(name);
}

// 셰이더 타입의 이름과 해시다(공개 헤더에 두는 식별 상수).
struct ShaderTypeName {
    StringView name;
    uint64 hash = 0;
};

// 셰이더 타입의 소속이다(SSD3). 라이브러리를 정한다: Engine·EnginePlugin → Engine 라이브러리,
// Project·ProjectPlugin → Project 라이브러리(UE와 같다). 원문 가상 경로의 루트와 맞아야 한다:
// Engine → /Engine, EnginePlugin·ProjectPlugin → /Plugin/<이름>, Project → /Project.
enum class ShaderTypeOwner : uint8 {
    Engine,
    EnginePlugin,
    Project,
    ProjectPlugin
};

// 셰이더 타입 표지 비트다(SSD2).
enum ShaderTypeFlagBits : uint32 {
    // 머티리얼 쿠킹이 머티리얼 셰이더 집합마다 곱한다(12.2절).
    ShaderTypeMaterialShader = 1u << 0,
    // 정점 입력 타입마다 따로 컴파일한다(12.2절, RenderBase SX4).
    ShaderTypeVertexInputAxis = 1u << 1,
    // 타입 전체가 개발 전용이다(7.2절 12항 — PS 진단 버퍼).
    ShaderTypeDevelopmentOnly = 1u << 2
};

// 모든 표지 비트다.
inline constexpr uint32 AllShaderTypeFlags =
    ShaderTypeMaterialShader | ShaderTypeVertexInputAxis | ShaderTypeDevelopmentOnly;

// HLSL 2021 선택이다(결정 2 — 형식 기본값을 셰이더 타입이 바꿀 수 있다).
enum class ShaderHlsl2021 : uint8 {
    FormatDefault,
    On,
    Off
};

// 정점 입력 타입 id(이름 해시, 없음 = 0)다. 정점 입력 타입은 셰이더 타입 등록부가 함께 받는다
// (ShaderTypeRegistry::RegisterVertexInputTypes — RenderBase 10.6절 1항).
using VertexInputTypeId = uint64;
inline constexpr VertexInputTypeId NoVertexInputType = 0;

struct ShaderFormatFacts;

// 셰이더 타입의 거르기 함수다(12.2절 — 순수 함수, 인자와 형식 사실만 읽는다). 엔진 셰이더 서명이다.
using ShaderShouldCompileFunction = bool (*)(VertexInputTypeId vertexInputType,
                                             uint32 permutationId,
                                             const ShaderFormatFacts& format) noexcept;

// 머티리얼 셰이더 타입(ShaderTypeMaterialShader)의 거르기 함수다(16.5절 SSD86, UE
// FMaterialShaderPermutationParameters 대응 — 순수 함수). materialTraits는 MaterialShaderTraits
// 정규 바이트(MaterialSchema WriteMaterialShaderTraits)이고 ShaderCore는 해석하지 않는다. 해석은
// pass 타입을 정의하는 모듈(RendererShaders)이 MaterialSchema 읽기로 한다. 쿠커가 넘기기 전에
// 바이트를 검사하므로 늘 읽힌다 — 그래도 읽지 못하면(계약 위반) Log Error와 false다.
//
// 사용 예(RendererShaders):
//   bool BasePassShouldCompile(Span<const uint8> traitsBytes, VertexInputTypeId, uint32,
//                              const ShaderFormatFacts& format) noexcept {
//       MaterialShaderTraits traits;
//       if (!ReadMaterialShaderTraits(traitsBytes, traits)) { Log::Error(...); return false; }
//       return traits.domain == MaterialDomain::Surface;
//   }
using ShaderMaterialShouldCompileFunction = bool (*)(Span<const uint8> materialTraits,
                                                     VertexInputTypeId vertexInputType,
                                                     uint32 permutationId,
                                                     const ShaderFormatFacts& format) noexcept;

// 셰이더 타입 기술이다(읽기 전용 값, 정적 수명). MakeShaderTypeDesc로 만든다.
struct ShaderTypeDesc {
    // 이름(ASCII [A-Za-z0-9_], 전역 유일)과 그 해시(ShaderTypeNameHash(name)과 같아야 한다)다.
    StringView name;
    uint64 nameHash = 0;
    // 엔트리 원문의 가상 경로(.rsf)와 엔트리 함수 이름이다.
    StringView sourcePath;
    StringView entryPoint;
    // ShaderStageBits 한 비트다(SC8).
    uint32 stage = 0;
    // 요구 형식 기능 비트(ShaderFeatureBits)다.
    uint32 requiredFeatures = 0;
    // ShaderTypeFlagBits다.
    uint32 flags = 0;
    ShaderTypeOwner owner = ShaderTypeOwner::Engine;
    ShaderHlsl2021 hlsl2021 = ShaderHlsl2021::FormatDefault;
    // 퍼뮤테이션 도메인이다(차원이 없으면 퍼뮤테이션 하나).
    ShaderPermutationDomain permutation;
    // 엔진 셰이더 거르기 함수다(null이면 늘 참). ShaderTypeMaterialShader 타입은 쓰지 않는다.
    ShaderShouldCompileFunction shouldCompile = nullptr;
    // 머티리얼 셰이더 거르기 함수다(null이면 늘 참 — 16.5절 SSD86). ShaderTypeMaterialShader 타입만
    // 쓴다.
    ShaderMaterialShouldCompileFunction materialShouldCompile = nullptr;
    // 손으로 쓴 파라미터 레이아웃 표 목록이다(7.2절 5항, SSD27 — 0개 이상). 서명이 job 키 8행과
    // 라이브러리 항목의 레이아웃 서명이다.
    Span<const ShaderParameterLayout> parameterLayouts;
};

// 정점 입력 타입의 거르기 함수다(SSD5 (4) — 정점 입력 축 타입에만 부른다, 순수 함수).
using ShaderVertexInputShouldCompileFunction = bool (*)(const ShaderTypeDesc& type,
                                                        uint32 permutationId,
                                                        const ShaderFormatFacts& format) noexcept;

// 정점 입력 축 셰이더가 include하는 고정 가상 경로다(RenderBase 10.6절 2항). 셰이더 컴파일러가 정점
// 입력 축 job마다 그 정점 입력 타입의 sourcePath로 잇는다(실제 파일이 아니다 — 연결 없는 job의
// include는 GeneratedSource 오류).
inline constexpr char VertexInputGeneratedIncludePath[] = "/Engine/Generated/VertexInput.rsh";

// 머티리얼 pass 셰이더가 include하는 생성 원문 가상 경로다(Docs/Design/MaterialSystem 7.2·7.8절,
// MSR3). 셰이더 컴파일러가 job의 materialSource 텍스트로 잇는다(연결 없는 job의 include는 전처리
// 오류).
inline constexpr char MaterialGeneratedIncludePath[] = "/Engine/Generated/Material.rsh";

// 정점 입력 타입 기술이다(RenderBase 10.2·10.6절, UE FVertexFactoryType 대응 — 셰이더 쪽 부분만:
// 장치 쪽 정적 정점 배치는 RenderBase가 가진다). 읽기 전용 값이며 가리키는 문자열은 정적 수명이다.
// MakeShaderVertexInputType으로 만든다. ShaderTypeRegistry::RegisterVertexInputTypes가 검사한다.
//
// 사용 예:
//   ROCKS_SHADER_TYPE_NAME(BuiltinVertexInputName, "BuiltinVertexInput");
//   constexpr ShaderVertexInputType Inputs[] = {MakeShaderVertexInputType(
//       BuiltinVertexInputName, "/Engine/Private/VertexInput/BuiltinVertexInput.rsh",
//       ShaderTypeOwner::Engine)};
struct ShaderVertexInputType {
    // 이름 해시(ShaderTypeNameHash(name))다. NoVertexInputType은 쓸 수 없다.
    VertexInputTypeId id = NoVertexInputType;
    // 이름(셰이더 타입 이름과 같은 문법)이다.
    StringView name;
    // 원문(.rsh 가상 경로)이다. 정점 입력 축 job의 VertexInputGeneratedIncludePath가 이 파일이다.
    StringView sourcePath;
    // 소속이다(셰이더 타입과 같은 루트 규칙).
    ShaderTypeOwner owner = ShaderTypeOwner::Engine;
    // 거르기(null이면 늘 참)다.
    ShaderVertexInputShouldCompileFunction shouldCompile = nullptr;
};

// 이름·해시 상수에서 정점 입력 타입 desc를 만든다.
constexpr ShaderVertexInputType
MakeShaderVertexInputType(ShaderTypeName name, StringView sourcePath, ShaderTypeOwner owner,
                          ShaderVertexInputShouldCompileFunction shouldCompile = nullptr) noexcept {
    ShaderVertexInputType input;
    input.id = name.hash;
    input.name = name.name;
    input.sourcePath = sourcePath;
    input.owner = owner;
    input.shouldCompile = shouldCompile;
    return input;
}

// 이름·해시 상수에서 desc를 만든다(나머지 칸은 기본값 — 필요한 칸을 이어서 바꾼다).
constexpr ShaderTypeDesc MakeShaderTypeDesc(ShaderTypeName name, StringView sourcePath,
                                            StringView entryPoint, uint32 stage,
                                            ShaderTypeOwner owner) noexcept {
    ShaderTypeDesc desc;
    desc.name = name.name;
    desc.nameHash = name.hash;
    desc.sourcePath = sourcePath;
    desc.entryPoint = entryPoint;
    desc.stage = stage;
    desc.owner = owner;
    return desc;
}

// 소속의 이름이다("Engine"·"EnginePlugin"·"Project"·"ProjectPlugin", 정적 수명). 범위 밖이면
// "unknown".
ROCKS_SHADERCORE_API const char* ShaderTypeOwnerName(ShaderTypeOwner owner) noexcept;

} // namespace rocks::shader

// 셰이더 타입의 이름·해시 상수 Variable(rocks::shader::ShaderTypeName)을 정의한다. 공개 헤더에 둔다
// (읽기 전용 inline constexpr — 쓰기 가능한 정적 저장소가 아니다).
#define ROCKS_SHADER_TYPE_NAME(Variable, NameLiteral)                                              \
    inline constexpr ::rocks::shader::ShaderTypeName Variable {                                    \
        ::rocks::StringView(NameLiteral),                                                          \
            ::rocks::shader::ShaderTypeNameHash(::rocks::StringView(NameLiteral))                  \
    }
