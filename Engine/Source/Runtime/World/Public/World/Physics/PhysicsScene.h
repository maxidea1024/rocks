#pragma once

#include <Base/Types.h>
#include <Core/Math/Transform.h>
#include <Core/Result.h>
#include <World/Physics/BodyCreateDesc.h>
#include <World/Physics/BodyKinematics.h>
#include <World/Physics/PhysicsBodyHandle.h>
#include <World/Physics/TeleportMode.h>

namespace rocks {

// 물리 장면의 진단 수치다(PhysicsScene::Stats). 값은 호출 시점 스냅샷이다.
struct PhysicsSceneStats {
    // 장면에 있는 body 수와 그중 깨어 있는 body 수다.
    uint32 bodies = 0;
    uint32 activeBodies = 0;
    // 장면을 만든 뒤 진행한 step 수다.
    uint64 steps = 0;
};

// World가 부르는 물리 장면 접점이다(Docs/Design/PhysicsModule 2.2절 — UE UWorld::GetPhysicsScene이
// 돌려주는 자리). World 모듈은 물리 엔진을 모르므로 이 추상 클래스만 알고, 구현은 Physics 모듈의
// Jolt 장면이다. 접점은 Physics 모듈의 PhysicsWorldSubsystem이 World 초기화 때 꽂고(World::
// SetPhysicsScene) 정리 때 뗀다. 장면이 없는 World(물리 없이 만든 시험·도구 World)에서
// World::GetPhysicsScene()은 nullptr이다.
//
// 함수는 구현하는 단계마다 더한다(PH1 Stats, PH2b body 수명·이동·운동 읽기 — PhysicsModule 7.1.1·
// 7.1.3절 PMD22). 공개 인자는 Rocks 단위(cm·kg·초)이고 결과는 값, 실패는 Result다.
//
// 사용 예:
//   if (PhysicsScene* scene = world.GetPhysicsScene()) {
//       PhysicsSceneStats stats = scene->Stats();
//   }
//
// 스레드: World의 게임 스레드에서만 부른다(PhysicsModule 3.2절).
class PhysicsScene {
public:
    virtual ~PhysicsScene() = default;

    // 장면의 body·step 수다.
    virtual PhysicsSceneStats Stats() const noexcept = 0;

    // body를 만들어 장면에 넣는다(2.3절 — PrimitiveComponent::CreateSystemState(Physics)가 부른다).
    // 오류: 형상 없음·잘못된 값(InvalidArgument), 장면 body 용량 초과(CapacityExceeded), 할당 실패.
    virtual Result<PhysicsBodyHandle> CreateBody(const BodyCreateDesc& desc) noexcept = 0;

    // body를 장면에서 빼고 지운다. 없는 handle·옛 세대는 무시한다.
    virtual void DestroyBody(PhysicsBodyHandle body) noexcept = 0;

    // body를 월드 transform(cm)으로 옮긴다(3.3절). scale은 쓰지 않는다(형상 갱신은 재생성 —
    // PH2d). 없는 handle·옛 세대는 무시한다.
    virtual void SetBodyTransform(PhysicsBodyHandle body, const Transform& worldTransform,
                                  TeleportMode mode) noexcept = 0;

    // body의 현재 운동 상태다. 오류: 없는 handle·옛 세대(StaleHandle).
    virtual Result<BodyKinematics> ReadKinematics(PhysicsBodyHandle body) const noexcept = 0;

protected:
    PhysicsScene() noexcept = default;
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;
};

} // namespace rocks
