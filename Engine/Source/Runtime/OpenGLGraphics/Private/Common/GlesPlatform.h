#pragma once

#include "GlesApi.h"
#include "GlesFormats.h"

#include <Core/Memory/UniquePtr.h>
#include <Graphics/FenceTimeline.h>
#include <OpenGLGraphics/OpenGLDevice.h>

// OpenGL ES 장치의 플랫폼 층이다(설계: Docs/Design/OpenGLESGraphics.ko.md 4절). context 생성·
// current 전환·창 표면·present·fence 대기·클립 공간 설정처럼 WGL·EGL에 따라 다른 일만 맡고, 나머지
// 장치 동작은 공통 코드(GlesDevice)가 ES 3.2 API로 한다. 데스크톱 GL 전용 호출(glClipControl,
// GL_FRAMEBUFFER_SRGB 켜기, WGL sRGB 픽셀 형식)은 Windows 플랫폼 층에만 있다.
namespace rocks::graphics::gles {

// 클립 공간과 화면 방향을 맞추는 방법이다(ShaderSystem 7.2절 8항). 셰이더 형식과 짝이다.
enum class ClipConvention : uint8 {
    // glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)으로 D3D 클립 공간을 그대로 받는다. 셰이더는
    // 보정하지 않는다(GLES31_PC: fixup_clipspace·flip_vert_y 끔). 앞면 감김은 GL_CCW다.
    ClipControlUpperLeft,
    // 셰이더가 VS 끝에서 y를 뒤집고 z를 [-1, 1]로 옮긴다(GLES31_ANDROID: SPIRV-Cross
    // fixup_clipspace·flip_vert_y). y가 뒤집혀 window 감김이 반대가 되므로 앞면은 GL_CW다.
    ShaderFlip
};

// 클립 공간 규약이 정하는 앞면 감김이다(Graphics 규약: 표시된 화면에서 반시계가 앞면).
//
// ClipControlUpperLeft: GL_UPPER_LEFT는 window y를 뒤집지만 ARB_clip_control·EXT_clip_control이 면
// 판정의 넓이 부호도 함께 뒤집으므로 감김은 NDC 기준 그대로이고 GL_CCW다(C7 측정 S5: 창·
// RenderTarget 모두 GL_CCW, UE도 clip control에서 기본 GL_CCW, OpenGLState.cpp:46–54).
// ShaderFlip: 셰이더가 clip y를 뒤집어 저장소 첫 행이 이미지 위쪽이 되고 감김이 반대가 된다(UE는
// clip control이 없으면 컬 방향을 뒤집는다, 같은 곳).
constexpr GLenum FrontFaceFor(ClipConvention convention) noexcept {
    return convention == ClipConvention::ClipControlUpperLeft ? GL_CCW : GL_CW;
}

// 플랫폼이 만든 context의 성질이다.
struct PlatformInfo {
    ClipConvention clip = ClipConvention::ClipControlUpperLeft;
    // 데스크톱 GL core context다(Windows 미리보기의 대체 경로). timestamp query 이름이 GL 3.3
    // core이고 GL_FRAMEBUFFER_SRGB 스위치가 core다.
    bool desktopContext = false;
    // "OpenGL ES 3.2 (WGL ES profile)" 같은 context 설명이다(adapter 이름 뒤에 붙인다).
    const char* contextName = "";
};

// 장치 생성 실패의 원인이다(DeviceCreateReport로 옮긴다). stage는 실패한 호출 이름(정적 문자열),
// nativeCode는 원본 오류(GL error, EGL error, Win32 GetLastError)다.
struct GlesCreateFailure {
    ErrorCode code = ErrorCode::None;
    const char* stage = "";
    int64 nativeCode = 0;
    Diagnostic detail;

    // 실패를 기록하고 code를 돌려준다.
    ErrorCode Set(ErrorCode error, const char* failedStage, int64 native,
                  const char* message) noexcept {
        code = error;
        stage = failedStage;
        nativeCode = native;
        detail = Diagnostic{};
        detail.Text(message);
        return error;
    }
};

// 창 표면 하나다(Windows: 창과 DC, Android: EGLSurface). 플랫폼 층만 내용을 안다.
using PlatformSurface = void*;

// 플랫폼 층이다. 장치 하나가 하나를 소유한다. 관찰 스레드의 fence 대기(FenceWaitSource)도 맡는다.
class GlesPlatform : public FenceWaitSource {
public:
    virtual ~GlesPlatform() = default;

    virtual const PlatformInfo& Info() const noexcept = 0;

    // context에서 얻은 ES 3.2 core 함수 표다(장치 수명 동안 유효).
    virtual const GlesFunctions& Gl() const noexcept = 0;

    // 이름으로 GL 함수 주소를 얻는다(확장 함수용). 없으면 nullptr이다.
    virtual void* ProcAddress(const char* name) noexcept = 0;

    // 장치 context를 이 스레드에 장치 표면(숨김 창·pbuffer·표면 없음)으로 붙인다.
    virtual bool MakeCurrent() noexcept = 0;

    // 장치 context가 이 스레드에 붙어 있는지다(표면은 묻지 않는다).
    virtual bool IsCurrent() const noexcept = 0;

    // 이 스레드에서 장치 context를 뗀다(붙어 있을 때만).
    virtual void ReleaseCurrent() noexcept = 0;

    // 창 표면을 만든다. 실패하면 error에 원인과 원본 오류를 적는다.
    virtual Result<PlatformSurface> CreateSurface(const NativeSurface& surface,
                                                  Diagnostic& error) noexcept = 0;

    // 장치 context를 창 표면에 붙인다(present blit은 그 표면의 기본 framebuffer에 그린다).
    virtual bool MakeCurrentOnSurface(PlatformSurface surface) noexcept = 0;

    // 창 표면의 지금 크기다. 알 수 없거나 0이면 false다.
    virtual bool SurfaceExtent(PlatformSurface surface, uint32& width, uint32& height) noexcept = 0;

    // 표면에 붙은 상태에서 버퍼를 바꿔 표시한다(vsync면 수직 동기). 실패하면 error에 적는다.
    virtual bool Present(PlatformSurface surface, bool vsync, Diagnostic& error) noexcept = 0;

    // 장치 context가 이 표면에 붙어 있으면 장치 표면으로 옮긴다(표면 파괴 전).
    virtual void DetachSurface(PlatformSurface surface) noexcept = 0;

    // 표면을 놓는다(그 표면을 쓴 제출의 fence가 끝난 뒤).
    virtual void DestroySurface(PlatformSurface surface) noexcept = 0;

    // 지금까지의 명령 뒤에 fence를 넣고 GPU로 보낸다(소유 스레드). 관찰 스레드가 WaitSubmission으로
    // 기다릴 token이다. 만들지 못하면 nullptr이며 호출자가 glFinish로 끝낸다.
    virtual void* InsertFence() noexcept = 0;

protected:
    GlesPlatform() = default;
};

// 이 플랫폼의 플랫폼 층을 만들고 context를 장치 표면에 붙인다(Win64: WGL, Android: EGL). 실패하면
// failure에 단계·원본 오류를 적고 그 code로 실패한다.
Result<UniquePtr<GlesPlatform>> CreateGlesPlatform(Allocator& allocator, const DeviceDesc& desc,
                                                   const OpenGLDeviceOptions& options,
                                                   GlesCreateFailure& failure) noexcept;

} // namespace rocks::graphics::gles
