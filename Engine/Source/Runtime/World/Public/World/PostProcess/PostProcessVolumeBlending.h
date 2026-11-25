#pragma once

#include <Core/Math/Vector3.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <World/PostProcess/PostProcessSettings.h>
#include <World/PostProcess/PostProcessVolumeTypes.h>

namespace rocks {

class World;

// 볼륨 정렬 순서다(UE PostProcessVolumeLess — World.cpp:10422): priority 오름차순 → unbound 먼저 →
// size 큰 것 먼저. 앞이면 참이다(마지막 동점은 BlendPostProcessVolumes가 handle 순으로 푼다).
ROCKS_WORLD_API bool PostProcessVolumeLess(const PostProcessVolumeProperties& first,
                                           const PostProcessVolumeProperties& second) noexcept;

// 볼륨 하나의 가중치다(UE DoPostProcessVolume — World.cpp:10697): 꺼졌으면 0, Clamp(blendWeight, 0,
// 1)에서 시작해 bounded면 거리 d로 d < 0·d > blendRadius는 0, blendRadius ≥ 1이면 × (1 − d /
// blendRadius)다. 결과가 [0, 1] 밖이면 0이다(UE ensure 뒤 0).
ROCKS_WORLD_API float PostProcessVolumeWeight(const PostProcessVolumeProperties& properties,
                                              const PostProcessEncompass& encompass) noexcept;

// World의 등록된(Initialized·Playing) PostProcessComponent를 모두 모아 정렬하고, viewLocation에서의
// 가중치로 차례로 settings를 덮는다(UE AddPostProcessingSettings — World.cpp:10764, Docs/Design/
// EngineRuntime 28.4절). 적용한(가중치 > 0) 볼륨 수를 돌려준다. scratch는 정렬 목록에 쓴다.
// 오류: OutOfMemory(settings는 바뀌지 않는다), 비유한 viewLocation(InvalidArgument).
//
// 사용 예(프레임마다 카메라 위치로):
//   PostProcessSettings view;   // 엔진 기본값
//   auto applied = BlendPostProcessVolumes(world, scratch, cameraLocation, view);
//
// 스레드: World의 게임 스레드 전용이다. 다른 World With 콜백 안에서 부르지 않는다.
ROCKS_WORLD_API Result<uint32> BlendPostProcessVolumes(World& world, Allocator& scratch,
                                                       const Vec3d& viewLocation,
                                                       PostProcessSettings& settings) noexcept;

} // namespace rocks
