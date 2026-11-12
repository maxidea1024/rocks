#pragma once

#include <Core/Text/String.h>
#include <Object/NativeClass.h>
#include <Object/ObjectRefArray.h>
#include <World/WorldTypes.h>

namespace rocks {

// World 안의 Actor 묶음이다. Level이 자기 Actor를 강한 GC 간선 목록으로 소유하고, World는 Level의
// 루트만 보유한다(World → Level → Actor → Component). World는 시작할 때 영구(persistent) Level을
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
// 객체 포인터는 World가 준 Borrow 범위에서만 쓴다. 스레드: World의 게임 스레드 전용이다.
class Level final : public Object {
    ROCKS_NATIVE_CLASS(Level, Object, 0x7e747b03964c4c42ull, 0x8914763d72829f13ull)

public:
    explicit Level(Allocator& allocator) noexcept : actors_(allocator), name_(allocator) {
    }

    // GC 강한 간선(소속 Actor)을 방문한다.
    void Trace(TraceVisitor& visitor) const noexcept {
        actors_.Trace(visitor);
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

    World* world_ = nullptr;
    ObjectRefArray<Actor> actors_;
    String name_;
    bool persistent_ = false;
    bool pendingDestroy_ = false;
};

} // namespace rocks
