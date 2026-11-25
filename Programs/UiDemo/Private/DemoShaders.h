#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <ModuleLoader/ModuleLoader.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/ShaderSet.h>

// UiDemo 셰이더 연결이다(Docs/Design/UiToolkit 9.1절 UW1a, RenderBase 9.12절 RR11). Sandbox
// self-test와 같은 순서다: 엔진 셰이더 호스트(render::EngineShaderHost)를 호스트 서비스로 붙이고
// 엔진 모듈 묶음을 시작하면 각 모듈이 셰이더 타입을 등록하고(RenderBase 엔진 타입, UiRender UI
// 타입) 개발 구성에서는 DevelopmentShaderProvider 모듈이 공급자를 붙인다. Freeze 뒤 장치마다
// render::EngineRenderResources로 셰이더 집합을 연다. Release 실행 파일에는 공급자가 없어 집합을
// 열지 못한다.
//
// 사용 예:
//   DemoShaderHost host(context.Modules());       // 프로세스 앞부분에서 한 번
//   DemoShaderSet shaders(allocator, device);      // 이 수명 동안 ActiveShaders()가 이 집합이다
//   auto solid = DrawBatchRenderer::CreateSolidPipeline(device, ActiveShaders(), format);
namespace rocks::ui {

class DemoShaderHost {
public:
    explicit DemoShaderHost(ModuleLoader& modules) noexcept;
    ~DemoShaderHost() noexcept;

    DemoShaderHost(const DemoShaderHost&) = delete;
    DemoShaderHost& operator=(const DemoShaderHost&) = delete;

    // 등록부를 Freeze했다(장치 집합을 열 수 있다).
    bool IsReady() const noexcept {
        return frozen_;
    }

private:
    ModuleLoader& modules_;
    bool attached_ = false;
    bool engineGroup_ = false;
    bool frozen_ = false;
};

class DemoShaderSet {
public:
    DemoShaderSet(Allocator& allocator, graphics::Device& device) noexcept;
    ~DemoShaderSet() noexcept;

    DemoShaderSet(const DemoShaderSet&) = delete;
    DemoShaderSet& operator=(const DemoShaderSet&) = delete;

    bool IsReady() const noexcept {
        return ready_;
    }

    // 열지 못한 이유다(EngineRenderResources 진단).
    StringView Problem() const noexcept;

private:
    graphics::Device& device_;
    render::EngineRenderResources resources_;
    render::ShaderSet* previous_ = nullptr;
    bool ready_ = false;
};

// 지금 열린 데모 셰이더 집합이다(가장 안쪽 DemoShaderSet). 데모는 집합을 연 뒤에만 그린다 — 열린
// 집합 없이 부르면 호출 계약 위반(assert)이다.
render::ShaderSet& ActiveShaders() noexcept;

} // namespace rocks::ui
