#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <ModuleLoader/ModuleLoader.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderFormatFacts.h>
#include <ShaderCore/ShaderType.h>

// Sandbox 셰이더다(Docs/Design/ShaderSystem 16.1절 SSD19·SH2f). 원문은 Programs/Sandbox/Shaders이고
// /Plugin/Sandbox로 mount한다(엔진 셰이더가 아니므로 Engine/Shaders에 두지 않는다). 셰이더 타입은
// 이 실행 파일만 등록하고 쿠킹하지 않는다. 등록부·공급자·장치별 집합은 엔진 호스트 공용
// 경로(Docs/Design/RenderBase 9.12절)를 쓴다: 이 실행 파일이 엔진 셰이더 호스트(EngineShaderHost)에
// Sandbox 타입과 원문 디렉터리를 넣어 서비스로 올리고, 엔진 셰이더 타입은 RenderBase 모듈이,
// 공급자는 Release 밖에서 링크한 DevelopmentShaderProvider 모듈이 Startup에서 넣는다. Release
// 실행 파일에는 공급자가 없으므로 셰이더 집합을 열지 못한다(UnsupportedCapability — 그리기
// self-test는 NOT RUN).
//
// 사용 예(self-test):
//   auto pipeline = shaders::CreateGraphicsPipeline(device, shaders::SampleVS, shaders::SamplePS,
//   desc);
namespace sandbox::shaders {

using rocks::StringView;
using rocks::uint32;
using rocks::shader::ShaderTypeDesc;

// Sandbox 셰이더 타입 기술이다(이름 "Sandbox<이름>", 소속 EnginePlugin, 원문
// /Plugin/Sandbox/<파일>).
constexpr ShaderTypeDesc SandboxType(StringView name, StringView path, StringView entry,
                                     uint32 stage) noexcept {
    return rocks::shader::MakeShaderTypeDesc(
        rocks::shader::ShaderTypeName{name, rocks::shader::ShaderTypeNameHash(name)}, path, entry,
        stage, rocks::shader::ShaderTypeOwner::EnginePlugin);
}

// 거르기 함수다(형식 상한 — ShaderSystem 7.2절 4항): VS·PS storage block을 받는 형식, render
// target 8개를 받는 형식.
constexpr bool NoGraphicsStorageBlocks(rocks::shader::VertexInputTypeId, uint32,
                                       const rocks::shader::ShaderFormatFacts& format) noexcept {
    return format.backend != rocks::shader::ShaderBackend::OpenGL;
}

constexpr bool EightRenderTargets(rocks::shader::VertexInputTypeId, uint32,
                                  const rocks::shader::ShaderFormatFacts& format) noexcept {
    return format.limits.renderTargets >= 8;
}

// D3D12 형식에서만 컴파일하는 타입이다(상수 버퍼 14개 — D3D12 형식 정의 상한, root 예산 시험).
constexpr bool D3D12Only(rocks::shader::VertexInputTypeId, uint32,
                         const rocks::shader::ShaderFormatFacts& format) noexcept {
    return format.backend == rocks::shader::ShaderBackend::D3D12;
}

constexpr ShaderTypeDesc Filtered(ShaderTypeDesc desc,
                                  rocks::shader::ShaderShouldCompileFunction filter) noexcept {
    desc.shouldCompile = filter;
    return desc;
}

// 개발 전용 타입이다(Development 공급자만 만든다 — PS면 진단 버퍼가 붙는다).
constexpr ShaderTypeDesc DevelopmentOnly(ShaderTypeDesc desc) noexcept {
    desc.flags |= rocks::shader::ShaderTypeDevelopmentOnly;
    return desc;
}

#define SANDBOX_VERTEX(Variable, File, Entry)                                                      \
    inline constexpr ::rocks::shader::ShaderTypeDesc Variable = SandboxType(                       \
        "Sandbox" #Variable, "/Plugin/Sandbox/" File, Entry, ::rocks::shader::ShaderStageVertex);
#define SANDBOX_PIXEL(Variable, File, Entry)                                                       \
    inline constexpr ::rocks::shader::ShaderTypeDesc Variable = SandboxType(                       \
        "Sandbox" #Variable, "/Plugin/Sandbox/" File, Entry, ::rocks::shader::ShaderStagePixel);
#define SANDBOX_COMPUTE(Variable, File, Entry)                                                     \
    inline constexpr ::rocks::shader::ShaderTypeDesc Variable = SandboxType(                       \
        "Sandbox" #Variable, "/Plugin/Sandbox/" File, Entry, ::rocks::shader::ShaderStageCompute);

#define SANDBOX_VERTEX_IF(Variable, File, Entry, Filter)                                           \
    inline constexpr ::rocks::shader::ShaderTypeDesc Variable =                                    \
        Filtered(SandboxType("Sandbox" #Variable, "/Plugin/Sandbox/" File, Entry,                  \
                             ::rocks::shader::ShaderStageVertex),                                  \
                 Filter);
#define SANDBOX_PIXEL_IF(Variable, File, Entry, Filter)                                            \
    inline constexpr ::rocks::shader::ShaderTypeDesc Variable =                                    \
        Filtered(SandboxType("Sandbox" #Variable, "/Plugin/Sandbox/" File, Entry,                  \
                             ::rocks::shader::ShaderStagePixel),                                   \
                 Filter);
#define SANDBOX_PIXEL_DEV_IF(Variable, File, Entry, Filter)                                        \
    inline constexpr ::rocks::shader::ShaderTypeDesc Variable =                                    \
        DevelopmentOnly(Filtered(SandboxType("Sandbox" #Variable, "/Plugin/Sandbox/" File, Entry,  \
                                             ::rocks::shader::ShaderStagePixel),                   \
                                 Filter));

#include "SandboxShaderTypes.inl"

#undef SANDBOX_VERTEX
#undef SANDBOX_PIXEL
#undef SANDBOX_COMPUTE
#undef SANDBOX_VERTEX_IF
#undef SANDBOX_PIXEL_IF
#undef SANDBOX_PIXEL_DEV_IF

// Sandbox가 등록하는 모든 셰이더 타입이다.
rocks::Span<const ShaderTypeDesc* const> AllTypes() noexcept;

// 프로세스의 엔진 셰이더 호스트다(Sandbox 타입·/Plugin/Sandbox 원문 디렉터리는 AttachHost가
// 넣는다).
rocks::render::EngineShaderHost& Host() noexcept;

// Sandbox 셰이더 타입과 원문 디렉터리를 넣고 호스트를 modules의 서비스로 올린다(모듈 단계 시작
// 전). 실패하면 Error 로그와 그 오류다. 한 프로세스에서 한 번 붙인다.
rocks::Result<void> AttachHost(rocks::ModuleLoader& modules) noexcept;

// 호스트를 서비스에서 뗀다(모든 묶음 종료 뒤).
void DetachHost(rocks::ModuleLoader& modules) noexcept;

// self-test의 호스트 범위다: 호스트를 붙이고 엔진 묶음 사용자 하나를 잡아(RenderBase·공급자 모듈
// 시작 — 등록부는 엔진 묶음 한 번의 수명이다, RenderBase 9.12절 1항) Freeze한다. 파괴하면 엔진
// 묶음을 놓고 호스트를 뗀다.
class SelfTestShaderHost {
public:
    explicit SelfTestShaderHost(rocks::ModuleLoader& modules) noexcept;
    ~SelfTestShaderHost() noexcept;
    SelfTestShaderHost(const SelfTestShaderHost&) = delete;
    SelfTestShaderHost& operator=(const SelfTestShaderHost&) = delete;

    bool IsReady() const noexcept {
        return frozen_;
    }

private:
    rocks::ModuleLoader& modules_;
    bool attached_ = false;
    bool engineGroup_ = false;
    bool frozen_ = false;
};

// 장치 하나의 엔진 렌더 자원(엔진 셰이더 집합 + 내장 자원 팩, RenderBase 9.12절 2항)이다. 만들 때
// 장치 소유 스레드에서 열고 활성 자원으로 둔다(이 실행 파일의 그리기 self-test는 활성 집합에서
// 셰이더를 고른다). 파괴하면 Retire한다.
class DeviceShaders {
public:
    DeviceShaders(rocks::Allocator& allocator, rocks::graphics::Device& device) noexcept;
    ~DeviceShaders() noexcept;
    DeviceShaders(const DeviceShaders&) = delete;
    DeviceShaders& operator=(const DeviceShaders&) = delete;

    // 열었다. 아니면 Problem 설명이 왜인지 말한다.
    bool IsReady() const noexcept;
    StringView Problem() const noexcept;

private:
    rocks::graphics::Device& device_;
    rocks::render::EngineRenderResources resources_;
    bool active_ = false;
};

// 활성 집합에 타입이 있는가(거르기로 빠진 타입은 없다).
bool Has(const ShaderTypeDesc& type) noexcept;

// 활성 집합이다(파이프라인 캐시 요청용). 없으면 null이다.
rocks::render::ShaderSet* ActiveSet() noexcept;

// 활성 내장 자원 팩이다. 없으면 null이다.
const rocks::render::BuiltinResources* ActiveBuiltins() noexcept;

// 활성 집합에서 타입의 정규화 바인딩 layout(값 1)이다(상수 버퍼 slot 찾기 — RenderBase 9.11절).
// 없으면 null이다.
const rocks::shader::ShaderBindingLayout* Layout(const ShaderTypeDesc& type) noexcept;

// 활성 집합의 장치 셰이더(첫 사용 생성)다. 활성 집합이 없으면 NotReady다.
rocks::Result<rocks::graphics::ShaderHandle> Get(const ShaderTypeDesc& type) noexcept;

// 활성 집합의 셰이더로 파이프라인을 만든다(desc의 셰이더 handle 칸은 이 함수가 채운다). 실패는
// 셰이더 찾기·생성 오류 또는 Device::CreatePipeline의 오류다.
rocks::Result<rocks::graphics::PipelineHandle>
CreateGraphicsPipeline(rocks::graphics::Device& device, const ShaderTypeDesc& vertex,
                       const ShaderTypeDesc& pixel, rocks::graphics::PipelineDesc desc) noexcept;
rocks::Result<rocks::graphics::PipelineHandle>
CreateComputePipeline(rocks::graphics::Device& device, const ShaderTypeDesc& compute,
                      rocks::graphics::ComputePipelineDesc desc) noexcept;

} // namespace sandbox::shaders
