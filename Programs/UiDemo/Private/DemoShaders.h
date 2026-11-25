#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <RenderBase/ShaderSet.h>

// UiDemo 셰이더 집합이다(Docs/Design/UiToolkit 9.1절 UW1a). 엔진 셰이더 타입(RenderBase
// EngineShaderTypes)과 UI 셰이더 타입(UiRender UiShaderTypes)을 등록하고, 쿠킹 라이브러리가
// 없으므로 Development 공급자(Developer DevelopmentShaderProvider)로 장치 형식의 라이브러리를
// 받는다. Release 실행 파일에는 공급자가 없어 집합을 열지 못한다(Sandbox SSD19와 같다).
//
// 사용 예:
//   DemoShaderSet shaders(allocator, device);   // 이 수명 동안 ActiveShaders()가 이 집합이다
//   auto solid = DrawBatchRenderer::CreateSolidPipeline(device, ActiveShaders(), format);
namespace rocks::ui {

class DemoShaderSet {
public:
    DemoShaderSet(Allocator& allocator, graphics::Device& device) noexcept;
    ~DemoShaderSet() noexcept;

    DemoShaderSet(const DemoShaderSet&) = delete;
    DemoShaderSet& operator=(const DemoShaderSet&) = delete;

    bool IsReady() const noexcept {
        return ready_;
    }

    // 열지 못한 이유다(등록 실패·공급자 진단).
    StringView Problem() const noexcept;

private:
    render::ShaderSet set_;
    render::ShaderSet* previous_ = nullptr;
    bool ready_ = false;
};

// 지금 열린 데모 셰이더 집합이다(가장 안쪽 DemoShaderSet). 데모는 집합을 연 뒤에만 그린다 — 열린
// 집합 없이 부르면 호출 계약 위반(assert)이다.
render::ShaderSet& ActiveShaders() noexcept;

// 프로세스 끝 전에 공급자의 워커를 멈춘다.
void ShutdownDemoShaders() noexcept;

} // namespace rocks::ui
