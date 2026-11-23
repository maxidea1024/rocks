# 엔진 컴포넌트 도입 — 1. 범위

[목차](README.ko.md)

## 1. 범위

### 1.1 대상

`EC/`의 헤더 73개 중 컴포넌트 클래스를 정의한 헤더 66개다. 나머지 7개는 컴포넌트가 아니다(2.4절).

### 1.2 폴더 밖(8절에서 다룬다)

`EC/` 밖에도 필수 컴포넌트가 있다. 개정 4에서 8절(2부)로 정리했다. 아래 표는 위치 안내다. 화면 마일스톤에 카메라가 필요하므로 `UCameraComponent`는 CM0과 함께 다룬다(4.3절 CR11).

| 위치 | 컴포넌트 | 메모 |
|---|---|---|
| `Classes/Camera` | `UCameraComponent`, `UCameraShakeSourceComponent` | 카메라는 화면 마일스톤 필수 |
| `Classes/GameFramework` | `UMovementComponent`·`UPawnMovementComponent`·`UNavMovementComponent`·`UCharacterMovementComponent`·`UProjectileMovementComponent`·`URotatingMovementComponent`·`UFloatingPawnMovement`·`USpectatorPawnMovement`, `USpringArmComponent` | 이동 계열. `UInterpToMovementComponent`(3.9절)의 부모 `UMovementComponent`도 여기 있다 |
| `Classes/PhysicsEngine`, `Classes/PhysicsField` | constraint·handle·spring·thruster·radial force·physical animation·cluster union·physics field | 물리 단계 |
| `Classes/Debug` | `UDebugDrawComponent` | 디버그 그리기 |
| `Classes/Particles` | `UParticleSystemComponent`(Cascade) | 제외(8.3절, CD5와 같은 이유) |
| Runtime 다른 모듈 | UMG `UWidgetComponent`(`Runtime/UMG/Public/Components/WidgetComponent.h`), Landscape, Foliage(`UFoliageInstancedStaticMeshComponent` — HISM 상속, `Runtime/Foliage/Public/FoliageInstancedStaticMeshComponent.h:20`), AIModule(Perception 등) | 각 기능 문서. Foliage는 CD2(HISM 흡수)의 영향을 받는다 — 식생 문서가 ISM 위에 다시 세운다 |
| 플러그인 | Niagara·Enhanced Input·AudioGameplayVolume·GameplayAbilities·PCG 등 | 9절(3부) |
