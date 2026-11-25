#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Core/TypeKey.h>
#include <World/Physics/PhysicsScene.h>
#include <World/World.h>

namespace rocks {

// World마다 물리 장면을 만들어 World에 꽂는 WorldSubsystem이다(Docs/Design/PhysicsModule 2.1절 —
// UE UWorld::PhysicsScene 생성·파괴). Initialize에서 Jolt 장면을 만들고(World 설정 문맥의
// PhysicsSettings, 없으면 기본값) World::SetPhysicsScene으로 꽂으며, Deinitialize에서 뗀 뒤
// 파괴한다. Subsystem 정리는 모든 Actor 정리 뒤라 body가 장면보다 먼저 사라진다.
//
// 만드는 World: Game·Tryout·Editor·Preview(Inactive 제외 — 편집기 picking·미리보기 충돌도 질의를
// 쓴다). step은 Game·Tryout World만 한다(3.1절 PMD16).
//
// 호스트가 World Subsystem 목록에 넣는다(Gameplay는 Physics에 의존하지 않는다 — 1.2절 PMD3):
//   config.worldSubsystems.TryPushBack(PhysicsWorldSubsystem::Type());
//
// 스레드: World의 게임 스레드 전용이다.
class ROCKS_PHYSICS_API PhysicsWorldSubsystem final : public WorldSubsystem {
public:
    explicit PhysicsWorldSubsystem(Allocator& allocator) noexcept;
    ~PhysicsWorldSubsystem() override;

    // World Subsystem 목록에 넣을 생성 정보다(이름 "Physics").
    static WorldSubsystemType Type() noexcept;

    // 장면을 만들어 World에 꽂는다. 오류: Jolt 버전 정의 불일치(InvalidArgument — 반입 정의와
    // 소비자 정의가 다르다), 설정 검증 실패(InvalidArgument), 할당 실패.
    Result<void> Initialize(World& world) noexcept override;

    // 장면을 World에서 떼고 파괴한다(진행 중인 job이 끝나기를 기다린다).
    void Deinitialize(World& world) noexcept override;

    // 이 World의 장면이다(Initialize 전·Deinitialize 뒤 nullptr — World::GetPhysicsScene과 같다).
    PhysicsScene* Scene() const noexcept {
        return scene_.Get();
    }

    // 이 World가 물리 step을 도는지다(Game·Tryout — Editor·Preview는 장면만 있다).
    bool Simulates() const noexcept {
        return simulates_;
    }

    // 고정 스텝 하나만큼 시뮬레이션한다(PhysicsSimulate 엔진 자리가 부른다 — 3.1절). seconds는 고정
    // 스텝 길이(초)다. 오류: 장면 없음(NotReady), step하지 않는 World(WrongContext), seconds가 양의
    // 유한값이 아님(InvalidArgument), Jolt 용량 초과(CapacityExceeded — 그 step은 진행했지만 일부
    // 쌍·접촉을 놓쳤다, 경고 로그).
    Result<void> SimulateStep(double seconds) noexcept;

private:
    Allocator& allocator_;
    UniquePtr<PhysicsScene> scene_;
    bool simulates_ = false;
};

} // namespace rocks

ROCKS_DECLARE_NATIVE_TYPE_KEY(rocks::PhysicsWorldSubsystem, ROCKS_PHYSICS_API)
