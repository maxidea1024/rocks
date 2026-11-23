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
    MapPopulate populate = nullptr;
    void* populateContext = nullptr;
};

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
