#pragma once

#include <Audio/AudioOutput.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>

// miniaudio 저수준 장치 API 위의 출력 backend다(Docs/Design/AudioSystem 3.34절). 공개 헤더에는
// miniaudio 타입을 두지 않는다(3.7.1절). Audio 모듈은 이 모듈을 링크하지 않고, 이 모듈의 수명
// 객체가 호스트의 AudioOutputBackendRegistry 서비스에 factory를 넣는다(AD24).
//
// 사용 예(호스트):
//   AudioOutputBackendRegistry registry;
//   (void)modules.Services().Register(registry);  // 모듈 시작 전에
//   (void)modules.StartEngineGroup();             // AudioMiniaudio Startup이 factory를 넣는다
//   UniquePtr<AudioOutputBackend> backend;
//   if (AudioOutputBackendFactory factory = registry.Factory()) {
//       if (auto made = factory(allocator)) {
//           backend = Move(made.Value());
//       }
//   }
//   auto device = AudioOutputDevice::TryOpen(config, Move(backend), request, {}, allocator);
namespace rocks::audio {

// MiniaudioOutput 하나를 만든다(장치는 Open에서 연다). allocator는 backend 객체와 miniaudio의
// context·device 할당에 쓰고 backend보다 오래 살아야 한다(miniaudio는 초기화 때만 할당한다).
//
// backend 동작(3.34.2절):
// - Open: 기본 장치 또는 request.device와 같은 id의 장치를 Float32·요청 채널 수·요청
//   rate(0이면 장치 기본)·주기 request.callbackFrames로 연다. 협상 callbackFrames는 장치 내부
//   주기다. 장치가 없으면 NotFound, 할당 실패는 OutOfMemory, 그 밖은 Unsupported이고 ma_result를
//   LastNativeError로 남긴다.
// - Stop은 장치 worker가 멈출 때까지 기다린다(돌아오면 콜백이 없다). Close는 Stop을 포함한다.
// - Name은 Open 뒤 실제 OS backend 이름(예: "WASAPI")이고, 그 전에는 "miniaudio"다.
ROCKS_AUDIOMINIAUDIO_API Result<UniquePtr<AudioOutputBackend>>
CreateMiniaudioOutput(Allocator& allocator) noexcept;

// registry가 비었으면 CreateMiniaudioOutput을 넣고 true다. 다른 factory가 이미 있으면 그대로 두고
// false다(호스트가 고른 backend를 존중한다).
ROCKS_AUDIOMINIAUDIO_API bool
RegisterMiniaudioOutput(AudioOutputBackendRegistry& registry) noexcept;

// registry의 factory가 CreateMiniaudioOutput일 때만 지운다.
ROCKS_AUDIOMINIAUDIO_API void
UnregisterMiniaudioOutput(AudioOutputBackendRegistry& registry) noexcept;

} // namespace rocks::audio
