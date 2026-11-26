#pragma once

#include <Gameplay/GameInstanceClient.h>
#include <World/World.h>

namespace sandbox {

// Sandbox 게임 설정이다(Docs/Design/EngineRuntime 21.5절). 기본 맵 "Main"은 PlayerStart 하나이고,
// 기본 GameDirector가 첫 LocalPlayer를 Login시켜 SandboxPawn을 준다. SandboxPawn은 Controller의
// 입력 스냅숏에서 새로 눌린 키·마우스 버튼을 로그로 남긴다(엔진 1차 목표 입력 확인 — 창 입력은
// 호스트가 SetLocalPlayerInput으로 넘긴다). 게임 World에는 물리 장면이 있다(PhysicsWorldSubsystem —
// Docs/Design/PhysicsModule 1.2절: Gameplay는 Physics에 의존하지 않으므로 호스트가 넣는다)과 오디오
// scene(AudioWorldSubsystem — 키 1–3 오디오 확인, Docs/Design/AudioSystem 3.35.11절)이 있다.
struct SandboxGame {
    // 게임 World Subsystem 수(PhysicsWorldSubsystem·AudioWorldSubsystem)다.
    static constexpr rocks::usize WorldSubsystemCount = 2;

    SandboxGame() noexcept;

    // config.worldSubsystems가 자기 멤버를 가리키므로 복사하지 않는다.
    SandboxGame(const SandboxGame&) = delete;
    SandboxGame& operator=(const SandboxGame&) = delete;

    // config.worldSubsystems가 가리키는 목록이다(config와 같은 수명).
    rocks::WorldSubsystemType worldSubsystems[WorldSubsystemCount];
    rocks::GameInstanceConfig config;
};

// SandboxPawn 등을 엔진 TypeRegistry에 등록한다(EngineConfig::registerTypes).
rocks::Result<void> RegisterSandboxTypes(rocks::TypeRegistry& types) noexcept;

} // namespace sandbox
