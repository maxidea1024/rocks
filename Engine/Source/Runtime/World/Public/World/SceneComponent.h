#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Transform.h>
#include <Meta/Annotations.h>
#include <World/Component.h>

namespace rocks {

// Transform 계층을 가진 Component다. 부모 기준 상대 Transform을 저장하고 월드 행렬은 필요할 때
// 계산한다(dirty 플래그). 렌더·물리·카메라 Component의 기반이다.
//
// 사용 예:
//   world.With<SceneComponent>(child, [&](SceneComponent& scene) {
//       scene.AttachTo(parent);                          // 순환이면 InvalidArgument
//       scene.SetRelativeLocation(Vec3d(0.0, 0.0, 100.0));
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
// - 월드 변환은 affine 행렬(Mat4d) = 부모 월드 행렬 × 상대 TRS 행렬이다. 비균일 scale 부모
//   아래 회전한 자식도 정확히 표현한다(TRS로 분해할 수 없는 shear 포함). TRS가 필요하면
//   TryWorldTransform을 쓴다.
// - 상대 Transform이 바뀌거나 부모가 바뀌면 자신과 모든 자손의 월드 행렬을 dirty로 표시한다.
// - 같은 World의 SceneComponent끼리만 붙일 수 있다. 자기 자신이나 자손을 부모로 지정하면 순환으로
//   거부한다. 부모가 파괴되면 자식은 루트가 된다(상대 Transform 유지).
// - 메서드는 부모·자식에 접근하기 위해 World Borrow를 새로 얻는다. 다른 SceneComponent의 With 콜백
//   안에서 호출해도 된다.
// - World 장부 밖 객체(HasWorld()가 거짓 — 그래프 로드 등)의 계층 API(AttachTo·Detach)는
//   InvalidArgument다. 상대 Transform 설정·월드 행렬 계산은 부모가 없으므로 된다.
ROCKS_CLASS()
class SceneComponent : public Component {
    ROCKS_GENERATED_BODY()

public:
    explicit SceneComponent(Allocator& allocator) noexcept : children_(allocator) {
    }

    const Transform& RelativeTransform() const noexcept {
        return relative_;
    }

    // 상대 Transform을 바꾼다. 유한하지 않거나 회전이 단위 quaternion이 아니면 InvalidArgument이며
    // 바꾸지 않는다.
    Result<void> SetRelativeTransform(const Transform& transform) noexcept;
    Result<void> SetRelativeLocation(const Vec3d& location) noexcept;
    Result<void> SetRelativeRotation(const Quat& rotation) noexcept;
    Result<void> SetRelativeScale(const Vec3& scale) noexcept;

    // 월드 행렬이다. dirty면 부모 사슬을 따라 다시 계산한다. 부모에 접근할 수 없으면(파괴 중 등)
    // 그 지점을 루트로 본다.
    Mat4d WorldMatrix() const noexcept;

    // 월드 변환을 TRS로 분해한다. shear가 있으면 Unsupported, 0 scale이면 InvalidArgument다.
    Result<Transform> TryWorldTransform() const noexcept;

    // 월드 위치(월드 행렬의 이동 성분)다.
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

private:
    friend class World;

    // 파괴 직전에 World가 부른다(등록 여부·파생 재정의와 무관). 부모에서 떼고 자식들을 루트로
    // 만든다(자식의 상대 Transform 유지, 월드 행렬은 다시 계산).
    void DetachForDestroy() noexcept;

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
};

} // namespace rocks
