#pragma once

#include <Core/Diagnostics/LogCategory.h>

namespace rocks::audio {

// 오디오 모듈 로그 범주다. AudioRT·장치 콜백에서는 쓰지 않는다(제어 쪽에서만,
// Docs/Design/AudioSystem 3.2절).
inline constexpr LogCategory LogAudio{"Audio"};

} // namespace rocks::audio
