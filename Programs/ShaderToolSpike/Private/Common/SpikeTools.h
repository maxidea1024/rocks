#pragma once

#include "SpikeCommon.h"

#include <ShaderToolLoader/ShaderToolLoader.h>

// 스파이크가 도구를 부르는 함수들이다. SPIRV-Tools·SPIRV-Cross·SPIRV-Reflect는 C API라 공통 코드
// (SpikeSpirv.cpp·SpikeReflect.cpp)가 구현하고, DXC(COM)·FXC·D3D12·WGL은 OS 헤더가 필요해
// Private/Win64가 구현한다(다른 플랫폼은 Private/Unsupported가 false·"unsupported"를 보고한다).
// 함수 표(shader::*Functions)는 ShaderToolSet이 연 것이며 그 객체가 열려 있는 동안만 부른다.
namespace rocks::spike {

// DXC 출력 종류다.
enum class DxcTarget : uint8 {
    // 두 출력 모두 -Zpr(행렬 행 우선)로 컴파일한다.
    // D3D12_SM6: DXIL(내장 validator 서명, -Qstrip_reflect로 리플렉션은 따로 받는다).
    Dxil,
    // VULKAN_SM5 이하 SPIR-V 경로: -spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-position-w.
    Spirv
};

// DXC 컴파일 결과다. code는 container(DXIL) 또는 SPIR-V word열, reflection은 DXIL의
// DXC_OUT_REFLECTION 부분(SPIR-V는 비어 있다), diagnostics는 DXC 오류·경고 문구다.
struct DxcOutput {
    DxcOutput() noexcept;

    ByteArray code;
    ByteArray reflection;
    StringBuilder<256> diagnostics;
};

// input을 DXC로 컴파일한다. shaderModel은 "6_0"처럼 프로필 뒤쪽이다(SPIR-V도 같은 프로필을 쓴다).
// 성공하면 true이고 경고는 diagnostics에 남는다.
bool CompileWithDxc(const shader::DxcFunctions& dxc, const ShaderInput& input, DxcTarget target,
                    const char* shaderModel, DxcOutput& output) noexcept;

// DXC 리플렉션 부분을 ID3D12ShaderReflection으로 열어 바인딩·상수 버퍼 오프셋·입출력 시맨틱을
// report의 열린 객체에 쓴다.
bool DumpDxilReflection(const shader::DxcFunctions& dxc, Span<const uint8> reflection,
                        Report& report) noexcept;

// FXC(D3DCompile, 고정본 d3dcompiler_47)로 DXBC를 만든다(/Zpr, 행렬 행 우선). name은 진단에 쓰는
// 가상 이름이다.
bool CompileWithFxc(const shader::FxcFunctions& fxc, StringView source, const char* name,
                    const char* entry, const char* profile, ByteArray& code,
                    StringBuilderBase& diagnostics) noexcept;

// 셰이더 요구 플래그(GetRequiresFlags, D3D_SHADER_REQUIRES_*)다(측정 S14).
struct ShaderRequires {
    uint64 flags = 0;
    // D3D_SHADER_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER: 장치의
    // VPAndRTArrayIndexFromAnyShaderFeedingRasterizer 기능이 있어야 이 셰이더를 쓴다.
    bool viewportAndLayerFromAnyShader = false;
};

// DXC 리플렉션 부분(ID3D12ShaderReflection)의 요구 플래그를 읽는다.
bool ReadDxilRequires(const shader::DxcFunctions& dxc, Span<const uint8> reflection,
                      ShaderRequires& result) noexcept;

// FXC DXBC를 D3DReflect(ID3D11ShaderReflection)로 열어 요구 플래그를 읽는다.
bool ReadDxbcRequires(const shader::FxcFunctions& fxc, Span<const uint8> code,
                      ShaderRequires& result) noexcept;

// 측정 S2: 하드웨어 어댑터와 WARP마다 D3D12 장치를 만들어 최대 셰이더 모델을 report에 쓴다.
// Agility SDK를 쓰지 않으므로(D3D12SDKVersion export 없음) OS D3D12 런타임의 값이다.
void ProbeD3D12ShaderModel(Report& report) noexcept;

// 측정 S12(데스크톱): 숨김 창 GL core context에서 GL_ARB_ES3_2_compatibility·GL_ARB_clip_control
// 유무를 보고, ESSL `#version 320 es` 원문(vertex·pixel)을 컴파일·링크한다. ES 3.2 context
// (WGL_EXT_create_context_es2_profile)가 되면 거기서도 같은 원문을 시험한다.
void ProbeOpenGl(StringView vertexEssl, StringView pixelEssl, Report& report) noexcept;

// SPIR-V 검증(spirv-val과 같은 spvValidateWithOptions, target env vulkan1.1)이다. 실패 문구는
// diagnostics에 남는다.
bool ValidateSpirv(const shader::SpirvToolsFunctions& tools, Span<const uint8> code,
                   StringBuilderBase& diagnostics) noexcept;

// SPIRV-Tools 성능 pass(spvOptimizerRegisterPerformancePasses)로 최적화한다(상수 접기 측정용).
bool OptimizeSpirv(const shader::SpirvToolsFunctions& tools, Span<const uint8> code, ByteArray& out,
                   StringBuilderBase& diagnostics) noexcept;

// SPIRV-Cross 출력 종류다(Docs/Design/ShaderSystem 2절 형식 표).
enum class CrossTarget : uint8 {
    // GLES31_ANDROID: ESSL 320, fixup_clipspace·flip_vert_y 켬.
    EsslAndroid,
    // GLES31_PC: ESSL 320, 둘 다 끔(Windows 미리보기는 클립 공간 보정을 glClipControl로 한다).
    EsslPc,
    // METAL_SM5: MSL 2.1 macOS, typed buffer는 texture_buffer(texture_buffer_native).
    Msl,
    // D3D11_SM5 대체 경로: HLSL 5.0(이 출력을 FXC로 다시 컴파일한다).
    Hlsl50
};

const char* CrossTargetName(CrossTarget target) noexcept;

// SPIR-V를 SPIRV-Cross로 바꾼다. 결과 원문은 text에 쓴다(앞 내용은 지운다). combinedSamplers가
// 있고 ESSL이면 SPIRV-Cross 결합 sampler 목록(결합·image·sampler 변수 이름)을 그 report의 열린
// 객체에 "combinedSamplers" 배열로 쓴다(측정 S13).
bool CrossCompile(const shader::SpirvCrossFunctions& cross, Span<const uint8> code,
                  CrossTarget target, StringBuilderBase& text, StringBuilderBase& diagnostics,
                  Report* combinedSamplers = nullptr) noexcept;

// ReportSpirvFeatures가 찾은 것이다.
struct SpirvFeatureMatch {
    bool capability = false;
    bool extension = false;
};

// SPIR-V 모듈의 OpCapability 번호와 OpExtension 이름을 report의 열린 객체에 "capabilities"(10진
// 문자열)·"extensions" 배열로 쓰고, capability·extension이 있는지 match에 남긴다(측정 S14).
bool ReportSpirvFeatures(Span<const uint8> code, uint32 capability, StringView extension,
                         Report& report, SpirvFeatureMatch& match) noexcept;

// SPIRV-Reflect로 descriptor binding(set·binding·종류·상수 블록 멤버 오프셋)·push constant·입출력
// 변수(location·built-in·이름)를 report의 열린 객체에 쓴다.
bool DumpSpirvReflection(Span<const uint8> code, Report& report) noexcept;

// 첫 storage image binding의 SPIR-V image format(SpvImageFormat 값)을 읽는다. 없으면 false다.
bool ReadStorageImageFormat(Span<const uint8> code, uint32& format) noexcept;

// SPIR-V에 OpExtInst(GLSL.std.450 초월 함수 호출 등)가 몇 개 남았는지 센다(상수 접기 판정용).
usize CountExtInst(Span<const uint8> code) noexcept;

} // namespace rocks::spike
