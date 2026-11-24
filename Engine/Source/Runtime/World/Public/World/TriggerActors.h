#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 영역 Trigger Actor의 기반이다(UE ATriggerBase — Docs/Design/EngineRuntime 24.3.7절). root가 Shape
// 컴포넌트다. 파생은 프레임워크 PreAwake에서 root를 만든다(광원 Actor와 같은 규칙: 같은 타입 root는
// 재사용, 다른 타입 root는 Spawn이 InvalidArgument). 만든 shape는 색 (100, 255, 100), profile
// "Trigger"이고 게임에서 숨는다(Shape 기본 hiddenInGame — UE는 Actor를 숨긴다). 데이터만 있다:
// overlap 이벤트는 물리(CR16)·이벤트(CR7) 때 붙는다.
//
// 사용 예:
//   auto zone = world.SpawnActor<TriggerBox>(SpawnParams::At(transform));
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API TriggerBase : public Actor {
    ROCKS_GENERATED_BODY()

public:
    // UE TriggerActors.cpp:20–21의 색·profile 이름과 shape 크기(cm, :28·:117·:179)다.
    static constexpr Color TriggerShapeColor = Color(100, 255, 100, 255);
    static constexpr const char* TriggerCollisionProfileName = "Trigger";
    static constexpr double TriggerBoxHalfExtent = 40.0;
    static constexpr float TriggerSphereRadius = 40.0f;
    static constexpr float TriggerCapsuleRadius = 40.0f;
    static constexpr float TriggerCapsuleHalfHeight = 80.0f;

    explicit TriggerBase(Allocator& allocator) noexcept : Actor(allocator) {
    }

    // root shape 컴포넌트다(PreAwake 전에는 null — UE GetCollisionComponent).
    ObjectHandle CollisionComponentHandle() const noexcept {
        return RootComponent();
    }
};

// 상자 Trigger다(UE ATriggerBox). root는 BoxComponent(extent 40)다.
ROCKS_CLASS()
class ROCKS_WORLD_API TriggerBox : public TriggerBase {
    ROCKS_GENERATED_BODY()

public:
    explicit TriggerBox(Allocator& allocator) noexcept : TriggerBase(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

// 구 Trigger다(UE ATriggerSphere). root는 SphereComponent(반지름 40)다.
ROCKS_CLASS()
class ROCKS_WORLD_API TriggerSphere : public TriggerBase {
    ROCKS_GENERATED_BODY()

public:
    explicit TriggerSphere(Allocator& allocator) noexcept : TriggerBase(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

// 캡슐 Trigger다(UE ATriggerCapsule). root는 CapsuleComponent(반지름 40·반 높이 80)다.
ROCKS_CLASS()
class ROCKS_WORLD_API TriggerCapsule : public TriggerBase {
    ROCKS_GENERATED_BODY()

public:
    explicit TriggerCapsule(Allocator& allocator) noexcept : TriggerBase(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
