#pragma once

#include <Base/Types.h>
#include <Core/Math/Vector3.h>

// 차폐 질의 공급자다(Docs/Design/AudioSystem 3.42.7절, UE FActiveSound::CheckOcclusion의 line trace
// 자리). World마다 AudioWorldSubsystem::SetOcclusionQuery로 단다. 물리 질의(PR16) 전에는 시험
// fixture가, 그 뒤에는 물리 line trace 공급자(Visibility 채널, owner actor 무시)가 구현한다.
//
// 스레드: 게임 스레드에서만 불린다(Play 수락 때 첫 검사, 프레임 시작 때 제어 쪽 요청 응답). 수명은
// 다는 쪽 책임이고 World 해제 전에 nullptr로 놓아야 한다(scene 해제가 함께 놓는다).
//
// 사용 예:
//   class WallQuery final : public AudioOcclusionQuery {
//       bool IsOccluded(const Vec3d& source, const Vec3d& listener, uint64 owner) noexcept
//       override;
//   };
//   world.GetSubsystem<AudioWorldSubsystem>()->SetOcclusionQuery(&query);
namespace rocks {

class AudioOcclusionQuery {
public:
    virtual ~AudioOcclusionQuery() noexcept = default;

    // 소리(source)와 리스너(listener) 사이가 막혔으면 true다(cm, world 좌표). owner는 무시할 소유
    // actor의 키(PlayOptions::owner — 0이면 없음)다.
    virtual bool IsOccluded(const Vec3d& source, const Vec3d& listener, uint64 owner) noexcept = 0;
};

} // namespace rocks
