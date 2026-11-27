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
//   OpenDemoShaders(resources, device);            // 렌더 lane에서 장치마다
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

// 장치의 엔진 셰이더 집합(UI 셰이더 포함)을 resources로 연다. 렌더 lane 처리기가 OnLaneStart에서
// 부른다(장치 소유 스레드). 실패: 호스트가 Freeze 전이거나 집합을 열지 못하면 NotReady(원인은
// resources.Problem()).
Result<void> OpenDemoShaders(render::EngineRenderResources& resources,
                             graphics::Device& device) noexcept;

} // namespace rocks::ui
