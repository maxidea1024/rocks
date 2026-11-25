#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/DeviceTypes.h>
#include <RenderBase/ShaderSet.h>

// 쿠킹 라이브러리가 없는 개발 실행 파일의 셰이더 공급자다(Docs/Design/ShaderSystem 16절 SSD19와
// 2026-11-25 공용화 메모). 셰이더 집합(render::ShaderSet::OpenFromProvider)에 넘기면 /Engine과
// 호스트가 준 원문 루트를 mount하고, 장치 형식마다 한 번 CompileShaderLibrary(너겟 캐시, 개발 전용
// 타입 포함)로 라이브러리를 만들어 넘긴다. 컴파일은 ShaderCompileWorker 프로세스가 한다 — 워커는
// 실행 파일 옆, 없으면 같은 구성의 ShaderCompileWorker 타깃 출력에서 찾는다.
//
// 사용 예(Sandbox):
//   const shader::ShaderSourceMountDesc mounts[] = {{"/Plugin/Sandbox",
//   "Programs/Sandbox/Shaders"}}; static shader::DevelopmentShaderProvider provider({.allocator =
//   &allocator, .mounts = mounts,
//                                                      .hostTarget = "sandbox"});
//   set.OpenFromProvider(&provider);
//
// 스레드: RequestLibrary는 셰이더 집합이 부르는 스레드에서 끝까지 한다(한 번에 하나). 수명:
// 공급자를 쓰는 셰이더 집합보다 오래 살아야 하고, 프로세스 끝 전에 Shutdown으로 워커를 멈춘다.
namespace rocks::shader {

// /Engine 밖에 더 mount할 원문 루트다. repositoryPath는 저장소 루트(엔진 폴더의 부모) 기준 상대
// 경로다.
struct ShaderSourceMountDesc {
    StringView virtualRoot = {};
    StringView repositoryPath = {};
};

struct DevelopmentShaderProviderDesc {
    Allocator* allocator = nullptr;
    Span<const ShaderSourceMountDesc> mounts = {};
    // 워커를 형제 출력(out/<도구>-<호스트>-<구성> → out/<도구>-shadercompileworker-<구성>)에서 찾을
    // 때 쓰는 호스트 타깃 이름(소문자 — 예 "sandbox")이다. 비면 실행 파일 옆만 찾는다.
    StringView hostTarget = {};
};

class ROCKS_DEVELOPMENTSHADERPROVIDER_API DevelopmentShaderProvider final
    : public render::ShaderProvider {
public:
    // 준비(mount·워커·너겟 캐시)는 처음 요청 때 한다. desc의 문자열·mount 표는 공급자보다 오래
    // 살아야 한다.
    explicit DevelopmentShaderProvider(const DevelopmentShaderProviderDesc& desc) noexcept;
    ~DevelopmentShaderProvider() noexcept;

    DevelopmentShaderProvider(const DevelopmentShaderProvider&) = delete;
    DevelopmentShaderProvider& operator=(const DevelopmentShaderProvider&) = delete;

    // 셰이더 집합의 요청이다. 실패하면 집합에 DependencyFailed와 진단(엔진 폴더·mount·워커·형식
    // 정의·컴파일 오류)을 넘기고 Success를 돌려준다. 할당 실패는 OutOfMemory다.
    Result<void> RequestLibrary(const render::ShaderProvisionRequest& request,
                                render::ShaderSet& set) noexcept override;

    // 워커와 너겟 캐시를 멈추고 만든 라이브러리를 놓는다(다시 부르면 다시 준비한다).
    void Shutdown() noexcept;

    struct State;

private:
    DevelopmentShaderProviderDesc desc_;
    UniquePtr<State> state_;
};

// 장치 backend의 Development 셰이더 형식 id다(SH2d 전 고정 표 — D3D12_SM6·D3D11_SM5·VULKAN_SM5·
// GLES31_PC·METAL_SM5).
ROCKS_DEVELOPMENTSHADERPROVIDER_API StringView
DevelopmentShaderFormat(graphics::Backend backend) noexcept;

} // namespace rocks::shader
