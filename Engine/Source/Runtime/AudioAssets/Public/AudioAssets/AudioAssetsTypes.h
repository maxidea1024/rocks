#pragma once

#include <Core/Result.h>

// AudioAssets 리플렉션 타입 등록이다(시험·도구가 엔진 없이 타입을 등록할 때 — 엔진은 모듈 타입
// 등록기로 자동 등록한다).
namespace rocks {

class ModuleRegistration;

// generated::AudioAssets::RegisterModuleTypes를 부른다. 오류는 그 함수와 같다.
ROCKS_AUDIOASSETS_API Result<void>
RegisterAudioAssetsTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
