#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringView.h>
#include <RenderBase/CookedManifestShaderLibraries.h>
#include <RenderBase/EngineShaderHost.h>

// 쿠킹 엔진 셰이더 라이브러리 self-test의 셰이더 호스트다(Docs/Design/ShaderSystem 16.1절 SSD48).
// 시험 호스트(Sandbox 시험 타입 + 공급자)와 따로, 쿠커와 같은 엔진 모듈 타입(RenderBase 엔진 셰이더
// 타입·정점 입력 타입)만 등록해 Freeze한 엔진 셰이더 호스트에 엔진 쿠킹 루트
// (`<엔진>/Saved/Cooked/<실행 프로파일>` — `asset-cook --engine --profile=<id>`의 결과)의
// 매니페스트 원천을 붙인다. 매니페스트가 없거나 읽지 못하면 준비되지 않았고 Reason이 이유를 말한다.
//
// 사용 예(장치마다):
//   CookedSelfTestShaders& cooked = CookedSelfTestShaders::Get();
//   if (!cooked.IsReady()) { printf("[NOT RUN] ... %s", cooked.Reason()); }
//   else { screen::ScreenHost screen(allocator, device, cooked.Host()); ... }
//
// 수명: 프로세스 하나에 하나(처음 Get이 만든다). 호스트는 서비스에 올리지 않는다(시험 호스트가
// 서비스 자리를 가진다).
namespace sandbox {

class CookedSelfTestShaders {
public:
    // 처음 부르면 준비한다(로더 스레드).
    static CookedSelfTestShaders& Get() noexcept;

    CookedSelfTestShaders(const CookedSelfTestShaders&) = delete;
    CookedSelfTestShaders& operator=(const CookedSelfTestShaders&) = delete;

    bool IsReady() const noexcept {
        return ready_;
    }

    // 준비되지 않은 이유다(영어).
    const char* Reason() const noexcept {
        return reason_;
    }

    const rocks::render::EngineShaderHost& Host() const noexcept {
        return host_;
    }

    // 쿠킹에 backend의 형식이 있는가(Windows 엔진 쿠킹에는 OpenGL이 없다 — GLES31_PC는 쿠킹하지
    // 않는다). 준비되지 않았으면 false다.
    bool Cooks(rocks::graphics::Backend backend) const noexcept;

private:
    explicit CookedSelfTestShaders(rocks::Allocator& allocator) noexcept;
    void Prepare() noexcept;

    // 이유 버퍼 byte 수다.
    static constexpr rocks::usize ReasonBytes = 512;

    rocks::Allocator& allocator_;
    rocks::render::EngineShaderHost host_;
    rocks::UniquePtr<rocks::render::CookedManifestShaderLibraries> source_;
    bool ready_ = false;
    char reason_[ReasonBytes] = {};
};

} // namespace sandbox
