#pragma once

#include <Gameplay/GameInstanceClient.h>

namespace sandbox {

// Sandbox 게임 설정이다(Docs/Design/EngineRuntime 21.5절). 기본 맵 "Main"은 PlayerStart 하나이고,
// 기본 GameDirector가 첫 LocalPlayer를 Login시켜 SandboxPawn을 준다. SandboxPawn은 Controller의
// 입력 스냅숏에서 새로 눌린 키·마우스 버튼을 로그로 남긴다(엔진 1차 목표 입력 확인 — 창 입력은
// 호스트가 SetLocalPlayerInput으로 넘긴다).
struct SandboxGame {
    SandboxGame() noexcept;

    rocks::GameInstanceConfig config;
};

// SandboxPawn 등을 엔진 TypeRegistry에 등록한다(EngineConfig::registerTypes).
rocks::Result<void> RegisterSandboxTypes(rocks::TypeRegistry& types) noexcept;

} // namespace sandbox
