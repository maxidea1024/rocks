#pragma once

#include <Graphics/Device.h>

namespace rocks::graphics {

// OpenGL 4.5 core 장치를 만든다. 숨김 창 위에 context를 만들고(WGL), 스왑체인은 같은 픽셀 형식(sRGB
// 지원, 더블 버퍼)을 창에 지정해 그 DC로 그린다. desc.enableValidation이면 debug context와 debug
// output을 켠다. desc.preferSoftware는 무시한다. 드라이버가 4.5를 지원하지 않거나 WGL 확장이 없으면
// DependencyFailed, Windows가 아니면 Unsupported다.
//
// 구현 규약:
// - glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)로 D3D와 같은 [0, 1] 깊이 범위를 쓴다.
// - 텍스처는 첫 행이 위인 배치로 올리며 UV (0,0)이 좌상단이다(D3D12와 같은 결과).
// - ReadPixels는 행 순서를 뒤집어 D3D12와 같은 위→아래 순서로 돌려준다.
// - 셰이더는 GLSL 4.50이다. 정점 속성은 layout(location = N), 상수는 uniform mat4 uTransform과
//   uniform vec4 uColor, 텍스처는 layout(binding = 0) uniform sampler2D uTexture다.
Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator& allocator, const DeviceDesc& desc) noexcept;

} // namespace rocks::graphics
