#pragma once

#include <Core/Diagnostics/LogCategory.h>
#include <Core/Text/StringView.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRegistry.h>
#include <World/World.h>

namespace rocks {

// Gameplay 모듈의 로그 카테고리다(Docs/Design/LoggingSystem.ko.md 13절). GameInstance 시작·종료와
// World 열기(OpenWorld)를 기록한다.
//
// 사용 예:
//   Log::Info(LogGameplay, "Opened world {Map}", name);
inline constexpr LogCategory LogGameplay{"Gameplay"};

// 진단 코드다(Docs/Design/EngineRuntime 22.1절 — 로그 {Code} 속성).
// - InvalidDirectorType: OpenWorld가 고른 GameDirector 타입이 GameDirector 파생이 아니다(Error).
// - PossessDeniedNoAuthority: 권한 없는(Client) World에서 Possess를 거부했다(Warning).
// - InputEvaluationFailed: PlayerController의 입력 액션 평가가 실패했다(다시 만들기 할당 실패 —
//   이전 목록으로 평가했다 — 또는 이벤트 기록 실패, Warning, 실패가 이어지는 동안 한 번 —
//   Docs/Design/InputActions 5.1절).
// - InputStackIncomplete: 입력 스택 쌓기가 할당 실패로 층을 빠뜨렸다(들어간 층만 전달, Error,
//   실패가 이어지는 동안 한 번 — InputActions 5.5절).
// - PlayerInputComponentFailed: Possess한 Pawn의 플레이어 입력 컴포넌트를 만들지 못했다(그 Pawn은
//   다시 Possess할 때까지 플레이어 입력이 없다, Error — 5.5절).
inline constexpr char DiagInvalidDirectorType[] = "InvalidDirectorType";
inline constexpr char DiagInputEvaluationFailed[] = "InputEvaluationFailed";
inline constexpr char DiagInputStackIncomplete[] = "InputStackIncomplete";
inline constexpr char DiagPlayerInputComponentFailed[] = "PlayerInputComponentFailed";
inline constexpr char DiagPossessDeniedNoAuthority[] = "PossessDeniedNoAuthority";

// 게임플레이 프레임워크 통계다(22.1절). 호출 시점의 World 하나 기준 값이다.
struct GameplayStats {
    uint32 controllers = 0;
    uint32 playerControllers = 0;
    uint32 pawns = 0;
    uint32 playerStates = 0;
};

// 실행 모드다(UE ENetMode, Docs/Design/EngineRuntime 18.3절). 네트워크가 없는 지금은
// Standalone과 DedicatedServer(LocalPlayer 없음)만 실행 경로가 있다.
enum class NetMode : uint8 {
    Standalone,
    DedicatedServer,
    ListenServer,
    Client
};

ROCKS_GAMEPLAY_API const char* NetModeName(NetMode mode) noexcept;

// 맵의 배치 Actor를 만드는 함수다(레벨 에셋 적재가 생기면 맵 에셋 id로 바뀐다 — 18.5절). World는
// Initialized(BeginPlay 전)다. 실패하면 OpenWorld가 그 World를 정리하고 실패를 돌려준다.
using MapPopulate = Result<void> (*)(World& world, void* context) noexcept;

// 열 맵이다(UE LoadMap의 URL 자리, 18.5절). 미룬 요청은 name을 복사해 두므로 호출이 끝나면 버려도
// 된다. populateContext는 OpenWorld가 실제로 실행될 때까지 살아 있어야 한다.
//
// 사용 예:
//   MapDesc map;
//   map.name = "Arena";
//   map.populate = [](World& world, void*) noexcept -> Result<void> {
//       auto spawned = world.SpawnActor<Spawner>();
//       return spawned ? Result<void>::Success() : Result<void>::Failure(spawned.Error());
//   };
//   auto opened = gameInstance.OpenWorld(map);
struct MapDesc {
    StringView name;
    WorldKind kind = WorldKind::Game;
    // 이 맵의 GameDirector 타입이다(비면 GameInstanceConfig::defaultDirectorType → GameDirector —
    // 19.3절).
    TypeId directorType;
    // "?key=value?flag" 형식 옵션이다(GameDirector::InitGame·Options, FindMapOption).
    StringView options;
    MapPopulate populate = nullptr;
    void* populateContext = nullptr;
};

// "?key=value?flag" 형식 맵 options에서 key의 값을 찾는다(UE UGameplayStatics::ParseOption — key는
// ASCII 대소문자 무시). 찾으면 true이고 value는 options 안의 범위다("?flag"처럼 값이 없으면 빈
// 값). 없으면 false이고 value는 그대로다.
//
// 사용 예:
//   StringView start;
//   if (FindMapOption("?start=North?rounds=3", "start", start)) { /* start == "North" */ }
ROCKS_GAMEPLAY_API bool FindMapOption(StringView options, StringView key,
                                      StringView& value) noexcept;

namespace detail {

// 엔진 Registry 객체를 T&로 빌려 function을 부른다(World::With와 같은 반환 규칙 — 포인터·참조·
// Span은 돌려줄 수 없다). 호출 동안 엔진 Registry의 ObjectAccessScope를 세운다.
template <class T, class Function>
auto WithEngineObject(ObjectRegistry& objects, ObjectHandle handle, Function&& function) noexcept {
    using Returned = decltype(function(DeclVal<T&>()));
    static_assert(!IsPointerV<Returned> && !IsReferenceV<Returned> && !IsSpanV<Returned>,
                  "Gameplay With must not return pointers, references, or Spans into the borrowed "
                  "object; copy the values out instead");
    using Output = typename WithResult<Returned>::Type;
    ObjectAccessScope scope(objects.OwnerAccess());
    auto borrowed = objects.Resolve<T>(objects.OwnerAccess(), handle);
    if (!borrowed) {
        return Output::Failure(borrowed.Error());
    }

    auto pointer = borrowed.Value().Get();
    if (!pointer) {
        return Output::Failure(pointer.Error());
    }

    if constexpr (IsSameV<Returned, void>) {
        function(*pointer.Value());
        return Output::Success();
    } else if constexpr (IsResultV<Returned>) {
        return function(*pointer.Value());
    } else {
        return Output::Success(function(*pointer.Value()));
    }
}

} // namespace detail

} // namespace rocks
