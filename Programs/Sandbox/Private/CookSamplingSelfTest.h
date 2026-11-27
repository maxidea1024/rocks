#pragma once

#include <Graphics/Device.h>

namespace sandbox {

// 쿠킹 보장 샘플링 표(Docs/Design/PixelFormat 7.1절)를 실제 장치 능력과 대조한다. 장치의 (backend,
// 이 실행 파일의 플랫폼)이 쿠킹 대상이면 표의 보장 칸마다 GetPixelFormatCaps가 그 능력을 가져야
// 한다(장치 생성이 같은 대조를 하므로 다시 확인하는 셈이다 — 어긋나면 실패). Windows의 OpenGL ES
// 미리보기는 쿠킹 대상이 아니므로 Android 열과 대조해 차이만 보고한다. 실패 수를 돌려준다.
rocks::uint32 RunCookSamplingChecks(const char* name,
                                    const rocks::graphics::Device& device) noexcept;

} // namespace sandbox
