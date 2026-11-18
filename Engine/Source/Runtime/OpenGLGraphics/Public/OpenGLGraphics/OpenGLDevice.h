#pragma once

#include <Graphics/Device.h>

namespace rocks::graphics {

// OpenGL backend의 context 종류다(Windows 미리보기만 고를 수 있다). 설계:
// Docs/Design/OpenGLESGraphics.ko.md 4.1절.
enum class OpenGLContextProfile : uint8 {
    // Windows: ES profile을 먼저 시도하고, 만들 수 없거나 요구 확장이 없으면 데스크톱 core로
    // 내려간다. Android: ES 3.2.
    Automatic,
    // OpenGL ES 3.2 context다(Windows: WGL_EXT_create_context_es2_profile, Android: EGL).
    // 드라이버가 ES API만 받으므로 공통 경로가 ES 3.2 밖 호출을 쓰면 debug output 오류가 된다.
    EsProfile,
    // 데스크톱 GL 4.3 core context에서 ES 3.2 공통 API와 `#version 320 es` 셰이더를 쓴다(Windows만,
    // UE의 GLES31_PC 미리보기와 같은 방식). GL_ARB_ES3_2_compatibility·GL_ARB_clip_control이
    // 필요하다. Android에서는 Unsupported다.
    DesktopCore
};

// CreateOpenGLDevice 선택 사항이다.
struct OpenGLDeviceOptions {
    OpenGLContextProfile contextProfile = OpenGLContextProfile::Automatic;
};

// OpenGL ES 3.2 장치를 만든다(ShaderSystem P11, 설계: Docs/Design/OpenGLESGraphics.ko.md). 모든
// 장치 동작은 ES 3.2 core API로 하고, 플랫폼 층만 context·창 표면·클립 공간을 맡는다.
//
// - Windows(GLES31_PC 미리보기): 숨김 창에 WGL context를 만든다(options.contextProfile). 클립
//   공간은 glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)이고(ES profile은 GL_EXT_clip_control,
//   데스크톱 core는 GL_ARB_clip_control), 이것이 없으면 Unsupported다. 개발용 미리보기이며 출시
//   Windows 실행 파일에 넣지 않는다(정책 release-windows-opengl).
// - Android(GLES31_ANDROID): EGL ES 3.2 context다. 셰이더가 클립 공간을 보정하고(SPIRV-Cross
//   fixup_clipspace·flip_vert_y) 장치는 앞면 감김을 뒤집는다. 창 표면은 P13에서 붙는다.
// - 그 밖의 플랫폼은 Unsupported다.
//
// desc.enableValidation이면 debug context와 debug output(KHR_debug)을 켠다. desc.preferSoftware는
// 무시한다.
//
// 실패: ES 3.2를 만들 수 없거나 필수 확장이 없으면 Unsupported, 그 밖의 OS·API 실패는
// DependencyFailed, 할당 실패는 OutOfMemory다. report가 있으면 실패한 단계와 원본 오류(GL error,
// EGL error, Win32 GetLastError)·설명을 적는다(성공이면 code만 None으로 둔다).
//
// 사용 예:
//   DeviceCreateReport report;
//   auto created = graphics::CreateOpenGLDevice(allocator, {.enableValidation = true}, &report);
//   if (!created) {
//       // report.stage, report.nativeCode, report.detail
//   }
//
// 구현 규약:
// - 셰이더는 GLSL ES 3.20(`#version 320 es`) 원문이다(SH2 전 런타임 컴파일, 쿠킹된 ESSL과 같은
//   모양). 정점 입력은 layout(location = N) in, 그리기별 상수는 루트 상수 에뮬레이션 배열
//   `uniform highp vec4 vu_h[N]`(VS)·`pu_h[N]`(PS)·`cu_h[N]`(CS)로 받는다(ShaderSystem 7.2절 4항,
//   N × 16 ≤ PipelineDesc::drawConstantBytes). 행렬은 행 우선 바이트를 그대로 올리므로 셰이더는
//   mat4(vu_h[0], ..., vu_h[3]) * v로 쓴다(열 우선으로 읽은 Mᵀ와 v의 곱 = v·M). 텍스처는
//   layout(binding = 0) uniform highp sampler2D ps0이다.
// - RenderTarget 텍스처의 저장소 첫 행이 이미지 위쪽이다(D3D12와 같은 배치). 스왑체인도 같은 배치의
//   offscreen 색 텍스처에 그리고 present 때 Y를 뒤집어 창에 blit한다(ShaderSystem 7.2절 8항).
// - 샘플러(TextureDesc::sampler)는 텍스처마다 샘플러 객체 하나다. Anisotropic은
//   GL_EXT_texture_filter_anisotropic이 있으면 적용하고(배율은 장치 한계로 줄인다) 없으면
//   Trilinear로 대신한다.
// - 앞면은 표시된 화면에서 반시계다(CullMode). 클립 공간 규약에 맞춘 glFrontFace는 장치가 정한다.
// - Windows 스왑체인 창의 클래스는 CS_OWNDC여야 한다(아니면 InvalidArgument).
// - timestamp는 GL_EXT_disjoint_timer_query(데스크톱 core는 GL 3.3 timer query)가 있을 때만
//   공개한다. 없으면 DeviceInfo::timestamps가 false다(추정값을 만들지 않는다).
Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator& allocator, const DeviceDesc& desc,
                                             DeviceCreateReport* report = nullptr) noexcept;

// options로 context 종류를 고르는 판이다. 나머지는 위와 같다.
Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator& allocator, const DeviceDesc& desc,
                                             const OpenGLDeviceOptions& options,
                                             DeviceCreateReport* report = nullptr) noexcept;

} // namespace rocks::graphics
