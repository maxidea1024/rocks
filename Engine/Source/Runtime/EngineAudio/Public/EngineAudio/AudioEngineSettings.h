#pragma once

#include <Core/Result.h>

// `audio.*` CVar 등록이다(Docs/Design/AudioSystem 3.35.10절). 모듈 RegisterSettings가 부르고,
// 호스트 없이 설정 문맥을 만드는 시험도 부른다.
namespace rocks {

namespace config {
class ConfigContext;
} // namespace config

// 오류는 ConfigContext::Register와 같다. 두 번 불러도 된다(같은 핸들 등록은 멱등).
ROCKS_ENGINEAUDIO_API Result<void>
RegisterEngineAudioSettings(config::ConfigContext& context) noexcept;

} // namespace rocks
