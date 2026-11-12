#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Transform.h>
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
// 규약:
// - 월드 변환은 affine 행렬(Mat4d) = 부모 월드 행렬 × 상대 TRS 행렬이다. 비균일 scale 부모
//   아래 회전한 자식도 정확히 표현한다(TRS로 분해할 수 없는 shear 포함). TRS가 필요하면
//   TryWorldTransform을 쓴다.
// - 상대 Transform이 바뀌거나 부모가 바뀌면 자신과 모든 자손의 월드 행렬을 dirty로 표시한다.
// - 같은 World의 SceneComponent끼리만 붙일 수 있다. 자기 자신이나 자손을 부모로 지정하면 순환으로
//   거부한다. 부모가 파괴되면 자식은 루트가 된다(상대 Transform 유지).
// - 메서드는 부모·자식에 접근하기 위해 World Borrow를 새로 얻는다. 다른 SceneComponent의 With 콜백
//   안에서 호출해도 된다.
class SceneComponent : public Component {
    ROCKS_NATIVE_TYPE(SceneComponent, Component, 0xf502dd5f77584a5dull, 0xba341911db1cacb6ull)

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
    // 자신·순환은 InvalidArgument, SceneComponent가 아니면 TypeMismatch, 다른 World·파괴된 대상은
    // StaleHandle, 자식 목록 할당 실패는 OutOfMemory이며 실패하면 기존 계층을 유지한다.
    Result<void> AttachTo(ObjectHandle parent) noexcept;

    // 부모에서 뗀다. 부모가 없으면 성공이다.
    Result<void> Detach() noexcept;

private:
    friend class World;

    // 파괴 직전에 World가 부른다(등록 여부·파생 재정의와 무관). 부모에서 떼고 자식들을 루트로
    // 만든다(자식의 상대 Transform 유지, 월드 행렬은 다시 계산).
    void DetachForDestroy() noexcept;

    void MarkDirty() noexcept;
    Result<void> AddChild(ObjectHandle child) noexcept;
    void RemoveChild(ObjectHandle child) noexcept;

    Transform relative_;
    mutable Mat4d world_;
    mutable bool worldDirty_ = true;
    ObjectHandle parent_;
    Array<ObjectHandle> children_;
};

} // namespace rocks
