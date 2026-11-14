#pragma once

#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefArray.h>
#include <World/Actor.h>
#include <World/WorldTypes.h>

namespace rocks {

// World 안의 Actor 묶음이다. Level이 자기 Actor를 강한 GC 간선 목록으로 소유하고, World는 Level의
// 루트만 보유한다(World -> Level -> Actor -> Component). World는 시작할 때 영구(persistent) Level을
// 하나 만들며, 서브 Level은 World::CreateLevel로 추가한다. 향후 레벨 스트리밍·월드 분할의
// 로드·언로드 단위다(이번 단계는 동기 생성·파괴만 지원한다).
//
// 사용 예:
//   auto dungeon = world.CreateLevel("Dungeon");
//   if (!dungeon) { ... }
//   SpawnParams params;
//   params.level = dungeon.Value();
//   auto orc = world.SpawnActor<Orc>(params);
//   auto unloaded = world.DestroyLevel(dungeon.Value());   // 소속 Actor를 정리한 뒤 Level 회수
//
// 리플렉션(Docs/Design/ReflectionFoundations.ko.md 9.1절): WorldOnly 리플렉션 클래스다. 영속
// 필드는 actors(소속 Actor 목록)·name이고 ReadOnly다(Spawn·정리는 World API로만 한다). World
// 포인터·영구 여부·파괴 대기는 ROCKS_NATIVE다. 객체 그래프(graph v1)로 Level을 저장할 수 있지만
// 구조 시험용이다 — 부착 계층(parent·children)은 저장되지 않고, 로드한 객체는 World 장부
// 밖이다(편입은 레벨 패키지 단계, EngineRuntime 문서 4.6절). 그래프에 native
// 파생(ROCKS_NATIVE_TYPE) Actor가 하나라도 있으면 저장은 Unsupported다. 저장 안전 지점은 Tick
// 밖·파괴 대기 정리 뒤다.
//
// 객체 포인터는 World가 준 Borrow 범위에서만 쓴다. 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(WorldOnly)
class Level final : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit Level(Allocator& allocator) noexcept : actors_(allocator), name_(allocator) {
    }

    // 소속 Actor handle 목록(Spawn 순서)이다. 이 Level의 Borrow 범위에서만 쓰며 Spawn·정리 후 다시
    // 얻는다.
    Span<const ObjectHandle> Actors() const noexcept {
        return actors_.View();
    }

    StringView Name() const noexcept {
        return name_.View();
    }

    bool IsPersistent() const noexcept {
        return persistent_;
    }

    // 파괴가 요청되어 정리를 기다리는 중인지다. 이 상태에서는 새 Actor를 Spawn할 수 없다.
    bool IsPendingDestroy() const noexcept {
        return pendingDestroy_;
    }

private:
    friend class World;

    ROCKS_NATIVE() World* world_ = nullptr;
    ROCKS_PROPERTY(ReadOnly, Required, Name = "actors") ObjectRefArray<Actor> actors_;
    ROCKS_PROPERTY(ReadOnly, Name = "name") String name_;
    ROCKS_NATIVE() bool persistent_ = false;
    ROCKS_NATIVE() bool pendingDestroy_ = false;
};

} // namespace rocks
