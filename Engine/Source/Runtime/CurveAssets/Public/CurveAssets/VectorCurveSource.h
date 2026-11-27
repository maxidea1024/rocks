#pragma once

#include <Core/Math/Vector3.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <CurveAssets/VectorCurveAsset.h>
#include <CurveAssets/VectorCurveValue.h>
#include <Meta/Annotations.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>

// 인라인 벡터 커브 또는 벡터 커브 에셋이다(Docs/Design/CurveAssets 3.1절, UE FRuntimeVectorCurve
// 대응). 참조 struct다 — 둘 수 있는 자리는 FloatCurveSource와 같다.
namespace rocks {

class ObjectRegistry;

// asset이 null이 아니면 그 에셋, null이면 curves를 쓴다.
//
// 사용 예:
//   auto offset = holder->motion.TryEvaluate(registry, access, time);
ROCKS_STRUCT()
struct ROCKS_CURVEASSETS_API VectorCurveSource {
    ROCKS_GENERATED_BODY()

    explicit VectorCurveSource(Allocator& allocator) noexcept : curves(allocator) {
    }

    bool HasAsset() const noexcept {
        return !asset.Handle().IsNull();
    }

    // 인라인 커브 값이다(참조는 보지 않는다).
    Vec3d EvaluateInline(float time) const noexcept {
        return curves.Evaluate(time);
    }

    // 쓰일 커브의 값이다. 오류는 FloatCurveSource::TryEvaluate와 같다.
    Result<Vec3d> TryEvaluate(ObjectRegistry& registry, const ObjectAccessContext& access,
                              float time) const noexcept;

    // 쓰일 커브를 out에 깊이 복사한다. 오류는 FloatCurveSource::TryFlattenInto와 같다(실패하면 out
    // 그대로).
    Result<void> TryFlattenInto(ObjectRegistry& registry, const ObjectAccessContext& access,
                                VectorCurveValue& out) const noexcept;

    ROCKS_PROPERTY() VectorCurveValue curves;
    ROCKS_PROPERTY() ObjectRef<VectorCurveAsset> asset;
};

} // namespace rocks
