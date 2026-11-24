#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Transform.h>
#include <Core/Text/String.h>
#include <Object/ObjectRefs.h>
#include <Meta/Annotations.h>
#include <World/Component.h>

namespace rocks {

// 부착할 때 Transform 성분 하나를 다루는 규칙이다(UE EAttachmentRule — Docs/Design/EngineRuntime
// 23.3절).
enum class AttachmentRule : uint8 {
    // 지금 상대 값을 새 부모 기준 상대 값으로 그대로 둔다.
    KeepRelative,
    // 붙이기 전 월드 값이 유지되도록 상대 값을 다시 계산한다.
    KeepWorld,
    // 부착점에 맞춘다(위치 0·회전 단위·scale 1).
    SnapToTarget
};

// 위치·회전·scale 성분마다의 부착 규칙이다(UE FAttachmentTransformRules).
struct AttachmentRules {
    AttachmentRule location = AttachmentRule::KeepRelative;
    AttachmentRule rotation = AttachmentRule::KeepRelative;
    AttachmentRule scale = AttachmentRule::KeepRelative;

    static constexpr AttachmentRules KeepRelativeTransform() noexcept {
        return {};
    }

    static constexpr AttachmentRules KeepWorldTransform() noexcept {
        return {AttachmentRule::KeepWorld, AttachmentRule::KeepWorld, AttachmentRule::KeepWorld};
    }

    // 위치·회전은 맞추고 scale은 월드 값을 유지한다(UE SnapToTargetNotIncludingScale).
    static constexpr AttachmentRules SnapToTargetNotIncludingScale() noexcept {
        return {AttachmentRule::SnapToTarget, AttachmentRule::SnapToTarget,
                AttachmentRule::KeepWorld};
    }

    static constexpr AttachmentRules SnapToTargetIncludingScale() noexcept {
        return {AttachmentRule::SnapToTarget, AttachmentRule::SnapToTarget,
                AttachmentRule::SnapToTarget};
    }
};

// SceneComponent가 움직일 수 있는지다(UE EComponentMobility). Static·Stationary는 게임 World에서
// 초기화 뒤 옮길 수 없다(23.3절).
ROCKS_ENUM()
enum class ComponentMobility : uint8 {
    Static,
    Stationary,
    Movable
};

// GetSocketTransform이 돌려줄 좌표계다.
enum class TransformSpace : uint8 {
    World,
    // socket을 가진 Component 기준이다.
    Component
};

// Component의 경계다(UE FBoxSphereBounds — 위치 cm). 상자는 origin ± boxExtent이고 구는 origin
// 중심 반지름 sphereRadius다.
struct ComponentBounds {
    Vec3d origin;
    Vec3d boxExtent;
    double sphereRadius = 0.0;
};

// Transform 계층을 가진 Component다. 부모 기준 상대 Transform을 저장하고 월드 행렬은 필요할 때
// 계산한다(dirty 플래그). 렌더·물리·카메라 Component의 기반이다.
//
// 사용 예:
//   world.With<SceneComponent>(child, [&](SceneComponent& scene) {
//       scene.AttachTo(parent);                          // 순환이면 InvalidArgument
//       scene.SetRelativeLocation(Vec3d(0.0, 0.0, 100.0)); // 부모 기준 위(+Z)로 100 cm(1 m)
//       // UE SetRelativeRotation(FRotator(P, Y, R))에 해당: 도 단위 Rotator를 바꿔 넘긴다.
//       scene.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion()); // yaw +90
//   });
//   auto matrix = world.With<SceneComponent>(
//       child, [](SceneComponent& s) { return s.WorldMatrix(); });
//
// 리플렉션(Docs/Design/ReflectionFoundations.ko.md 9.1절): Component의 WorldOnly를 물려받는
// 리플렉션 클래스다. 영속 필드는 relativeTransform(Core 값 타입 Transform — CoreTypeBindings 미러
// 경유)이고 ReadOnly다: 경로·리플렉션 쓰기는 MarkDirty·검증을 거치지 않으므로
// SetRelativeTransform만 쓴다(CU5 setter hook 전에는 에디터에서 경로로 편집할 수 없다 — UE
// EditAnywhere와 다른 제약). 월드 행렬 캐시·부모·자식은 ROCKS_NATIVE다 — 부착 계층은 저장하지
// 않는다(추적하지 않는 참조 WeakObjectRef가 생긴 뒤 레벨·prefab 저장 전에 영속한다, children은
// parent에서 유도되는 값이다).
//
// 규약:
// - 좌표·단위는 UE 5.8과 같다(Docs/Implementation/Math.ko.md 2절): 왼손 +X 전방·+Y 오른쪽·+Z 위,
//   위치·이동은 cm(UE WorldSettings.h:593-595 WorldToMeters 100), 회전은 단위 quaternion(UE
//   FQuat 의미 — 양의 각은 왼손 회전, 도 단위 각은 Rotator<double>::ToQuaternion으로 바꾼다),
//   scale은 무차원 배율이다. 물리 엔진 등 m 단위 계층과는 Core/Math/UnitConversion.h의 함수로
//   경계에서 바꾼다.
// - 월드 변환은 affine 행렬(Mat4d) = 상대 TRS 행렬 × 부모 월드 행렬이다(행 벡터 규약, UE
//   NewRelativeTransform * ParentToWorld — Engine/Private/Components/SceneComponent.cpp:732).
//   비균일 scale 부모 아래 회전한 자식도 정확히 표현한다(TRS로 분해할 수 없는 shear 포함 — UE
//   FTransform 곱은 shear를 버린다). TRS가 필요하면 TryWorldTransform을 쓴다.
// - 상대 Transform이 바뀌거나 부모가 바뀌면 자신과 모든 자손의 월드 행렬을 dirty로 표시한다.
// - 같은 World의 SceneComponent끼리만 붙일 수 있다. 자기 자신이나 자손을 부모로 지정하면 순환으로
//   거부한다. 부모가 파괴되면 자식은 루트가 된다(상대 Transform 유지).
// - 메서드는 부모·자식에 접근하기 위해 World Borrow를 새로 얻는다. 다른 SceneComponent의 With 콜백
//   안에서 호출해도 된다.
// - World 장부 밖 객체(HasWorld()가 거짓 — 그래프 로드 등)의 계층 API(AttachTo·Detach)는
//   InvalidArgument다. 상대 Transform 설정·월드 행렬 계산은 부모가 없으므로 된다.
ROCKS_CLASS()
class ROCKS_WORLD_API SceneComponent : public Component {
    ROCKS_GENERATED_BODY()

public:
    explicit SceneComponent(Allocator& allocator) noexcept
        : children_(allocator), attachSocket_(allocator) {
    }

    const Transform& RelativeTransform() const noexcept {
        return relative_;
    }

    // 상대 Transform을 바꾼다. 유한하지 않거나 회전이 단위 quaternion이 아니면 InvalidArgument이며
    // 바꾸지 않는다. 아래 세 함수는 해당 성분만 바꾸며 실패 조건이 같다.
    Result<void> SetRelativeTransform(const Transform& transform) noexcept;
    // 부모 기준 위치(cm)다. UE SetRelativeLocation과 같은 단위다.
    Result<void> SetRelativeLocation(const Vec3d& location) noexcept;
    // 부모 기준 회전(단위 quaternion, 왼손 양의 각)이다. 도 단위 Rotator는 ToQuaternion으로 바꾼다.
    Result<void> SetRelativeRotation(const Quatd& rotation) noexcept;
    // 부모 기준 scale(무차원 배율, 1 = 원래 크기)이다.
    Result<void> SetRelativeScale(const Vec3d& scale) noexcept;

    // 월드 행렬이다. dirty면 부모 사슬을 따라 다시 계산한다. 부모에 접근할 수 없으면(파괴 중 등)
    // 그 지점을 루트로 본다.
    Mat4d WorldMatrix() const noexcept;

    // 월드 변환을 TRS로 분해한다. shear가 있으면 Unsupported, 0 scale이면 InvalidArgument다.
    Result<Transform> TryWorldTransform() const noexcept;

    // 월드 위치(cm, 월드 행렬의 이동 성분 — 마지막 행)다.
    Vec3d WorldLocation() const noexcept;

    ObjectHandle Parent() const noexcept {
        return parent_;
    }

    Span<const ObjectHandle> Children() const noexcept {
        return children_.View();
    }

    // parent의 자식으로 붙인다(상대 Transform 유지). 이미 다른 부모가 있으면 먼저 뗀다. null·자기
    // 자신·순환·World 장부 밖 객체(이 객체나 parent)는 InvalidArgument, SceneComponent가 아니면
    // TypeMismatch, 다른 World·파괴된 대상은 StaleHandle, 자식 목록 할당 실패는 OutOfMemory이며
    // 실패하면 기존 계층을 유지한다.
    Result<void> AttachTo(ObjectHandle parent) noexcept;

    // 부모에서 뗀다. 부모가 없으면 성공이다. World 장부 밖 객체는 InvalidArgument다.
    Result<void> Detach() noexcept;

    // --- 부착 규칙·socket(23.3절 CR2) ---

    // parent의 socket(비면 parent 자신)에 rules로 붙인다(UE AttachToComponent). 오류: 위 AttachTo와
    // 같고, 표현할 수 없는 KeepWorld 계산(scale 0·shear 분해 실패)은 InvalidArgument(바뀌지
    // 않는다), Mobility 위반은 InvalidArgument(StaticComponentMoved). parent에 없는 socket 이름은
    // UE처럼 받아 두고 parent 자신의 Transform을 쓴다.
    Result<void> AttachTo(ObjectHandle parent, const AttachmentRules& rules,
                          StringView socket = {}) noexcept;

    // rule로 뗀다(KeepRelative는 상대 값을 월드 값으로 둔다 — 위 Detach와 같다, KeepWorld는 월드
    // Transform을 유지한다).
    Result<void> Detach(AttachmentRule rule) noexcept;

    // 부착한 socket 이름이다(없으면 빈 값).
    StringView AttachSocketName() const noexcept {
        return attachSocket_.View();
    }

    // name socket이 있는지다(기본은 없음 — 메시 계열이 재정의한다).
    virtual bool HasSocket(StringView) const noexcept {
        return false;
    }

    // socket의 Transform이다(UE GetSocketTransform). 이름이 비거나 없는 socket이면 Component
    // 자신이다(space가 Component면 단위). 오류: 월드 Transform 분해 실패.
    virtual Result<Transform> GetSocketTransform(StringView socket,
                                                 TransformSpace space) const noexcept;

    // --- 절대 Transform·Mobility(23.3절) ---

    // 켠 성분은 부모를 무시하고 상대 값을 월드 값으로 쓴다(UE SetAbsolute).
    void SetAbsolute(bool location, bool rotation, bool scale) noexcept;

    bool IsAbsoluteLocation() const noexcept {
        return absoluteLocation_;
    }

    bool IsAbsoluteRotation() const noexcept {
        return absoluteRotation_;
    }

    bool IsAbsoluteScale() const noexcept {
        return absoluteScale_;
    }

    ComponentMobility Mobility() const noexcept {
        return mobility_;
    }

    void SetMobility(ComponentMobility mobility) noexcept {
        mobility_ = mobility;
    }

    // --- 가시성(23.3절) ---

    // visible을 바꾼다(UE SetVisibility). propagateToChildren이면 자손에도 같은 값을 쓴다.
    void SetVisibility(bool visible, bool propagateToChildren = false) noexcept;

    // hiddenInGame을 바꾼다(UE SetHiddenInGame).
    void SetHiddenInGame(bool hidden, bool propagateToChildren = false) noexcept;

    bool GetVisibleFlag() const noexcept {
        return visible_;
    }

    bool IsHiddenInGame() const noexcept {
        return hiddenInGame_;
    }

    // 보이는지다: visible이고, 게임 World(Game·Tryout)면 hiddenInGame도 아니다(UE ShouldRender).
    bool IsVisible() const noexcept;

    // --- 속도·bounds(23.3절) ---

    // Component 속도(cm/s, UE ComponentVelocity)다. 이동 컴포넌트가 쓴다.
    const Vec3d& ComponentVelocity() const noexcept {
        return velocity_;
    }

    void SetComponentVelocity(const Vec3d& velocity) noexcept {
        velocity_ = velocity;
    }

    // 캐시한 경계다(월드 Transform이 바뀌면 다음 호출에서 CalcBounds로 다시 계산한다).
    const ComponentBounds& Bounds() const noexcept;

    // 월드 Transform에서 경계를 계산한다(UE CalcBounds). 기본은 위치의 점(extent·반지름 0).
    virtual ComponentBounds CalcBounds(const Transform& world) const noexcept;

private:
    friend class World;

    // 파괴 직전에 World가 부른다(등록 여부·파생 재정의와 무관). 부모에서 떼고 자식들을 루트로
    // 만든다(자식의 상대 Transform 유지, 월드 행렬은 다시 계산).
    void DetachForDestroy() noexcept;

protected:
    // 자신이나 조상의 Transform·부착이 바뀐 뒤 그 변경 호출이 끝날 때 한 번 불린다(UE
    // OnUpdateTransform — 자신 → 자손 순). Render 시스템 상태가 있으면 다시 보내기는 엔진이
    // 표시한다.
    virtual void OnTransformUpdated() noexcept {
    }

    // 가시성(visible·hiddenInGame)이 바뀌었다.
    virtual void OnVisibilityChanged() noexcept {
    }

private:
    // 게임 World에서 초기화 뒤 Static·Stationary면 InvalidArgument(Warning 진단)다.
    Result<void> CheckMovable(const char* action) const noexcept;
    // 자신과 자손에 OnTransformUpdated를 부르고 bounds·Render 상태를 표시한다.
    void NotifyTransformUpdated() noexcept;
    // 부모 socket의 월드 Transform이다(부모가 없으면 단위).
    Result<Transform> ParentSocketWorld() const noexcept;
    // 링크만 바꾼다(검사·규칙 계산 뒤).
    Result<void> LinkParent(ObjectHandle parent, StringView socket) noexcept;
    void ApplyVisibility(bool visible, bool hidden, bool setVisible, bool propagate) noexcept;

    void MarkDirty() noexcept;
    Result<void> AddChild(ObjectHandle child) noexcept;
    void RemoveChild(ObjectHandle child) noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "relativeTransform") Transform relative_;
    // 월드 행렬 캐시다(Component::world_를 가리지 않게 worldMatrix_라 한다).
    ROCKS_NATIVE() mutable Mat4d worldMatrix_;
    ROCKS_NATIVE() mutable bool worldDirty_ = true;
    ROCKS_NATIVE() ObjectHandle parent_;
    // parent_에서 유도되는 값이다(UE AttachChildren처럼 Transient — 편입이 다시 만든다).
    ROCKS_NATIVE() Array<ObjectHandle> children_;
    // 부착 부모·socket이다(영속 — 23.3절 CR3, 편입이 계층을 복원한다). parent_와 같은 값이다.
    ROCKS_PROPERTY(ReadOnly, Name = "attachParent") WeakObjectRef<SceneComponent> attachParent_;
    ROCKS_PROPERTY(ReadOnly, Name = "attachSocket") String attachSocket_;
    ROCKS_PROPERTY(ReadOnly, Name = "absoluteLocation") bool absoluteLocation_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "absoluteRotation") bool absoluteRotation_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "absoluteScale") bool absoluteScale_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "mobility")
    ComponentMobility mobility_ = ComponentMobility::Movable;
    ROCKS_PROPERTY(ReadOnly, Name = "visible") bool visible_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "hiddenInGame") bool hiddenInGame_ = false;
    ROCKS_NATIVE() Vec3d velocity_;
    ROCKS_NATIVE() mutable ComponentBounds bounds_;
    ROCKS_NATIVE() mutable bool boundsDirty_ = true;
};

} // namespace rocks
