#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <CurveAssets/FloatCurveAsset.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>

// 인라인 커브 또는 스칼라 커브 에셋이다(Docs/Design/CurveAssets 3.1절, UE FRuntimeFloatCurve 대응).
//
// 참조 struct다(ObjectRef — Hard): 리플렉션 클래스(에셋 객체)의 직접 필드나 StructRefArray 원소에만
// 둔다. Array 원소·Map 값·Optional 안·자유롭게 복사하는 값 struct에는 둘 수 없다 — 그런 자리는
// TryFlattenInto로 만든 TangentCurve를 쓴다(설계 CAD7·CAD9).
namespace rocks {

class ObjectRegistry;

// asset이 null이 아니면 그 에셋의 커브, null이면 curve를 쓴다(인라인은 참조가 있어도 남는다 — UE와
// 같다). asset은 ObjectRegistry::SetReference로 바꾼다.
//
// 사용 예(에셋 객체의 필드 — 소유 Registry로 평가):
//   auto value = holder->falloff.TryEvaluate(registry, access, distance);
//   if (!value) { return Result<void>::Failure(value.Error()); }
ROCKS_STRUCT()
struct ROCKS_CURVEASSETS_API FloatCurveSource {
    ROCKS_GENERATED_BODY()

    explicit FloatCurveSource(Allocator& allocator) noexcept : curve(allocator) {
    }

    bool HasAsset() const noexcept {
        return !asset.Handle().IsNull();
    }

    // 인라인 커브의 Evaluate(time)다(fallback 0). 참조는 보지 않는다.
    float EvaluateInline(float time) const noexcept {
        return curve.Evaluate(time);
    }

    // 쓰일 커브의 값이다: 참조가 null이면 인라인, 아니면 에셋의 Evaluate(time). registry·access는
    // 이 원천을 가진 객체의 Registry와 그 접근 문맥이다. 오류: 참조가 풀리지 않으면 Resolve의 오류
    // (파괴 요청된 대상 Destroyed — 원천이 강한 간선이라 대상은 회수되지 않는다, 다른 스레드
    // WrongThread) — 인라인으로 대신하지
    // 않는다(설계 CAD8).
    Result<float> TryEvaluate(ObjectRegistry& registry, const ObjectAccessContext& access,
                              float time) const noexcept;

    // 쓰일 커브(참조가 있으면 에셋 커브, 없으면 인라인)를 out에 깊이 복사한다(평탄화 — 사본이
    // Registry 없이 평가한다). out의 allocator는 그대로다. 오류: TryEvaluate의 Resolve 오류, 할당
    // 실패. 실패하면 out은 그대로다.
    Result<void> TryFlattenInto(ObjectRegistry& registry, const ObjectAccessContext& access,
                                TangentCurve& out) const noexcept;

    ROCKS_PROPERTY() TangentCurve curve;
    ROCKS_PROPERTY() ObjectRef<FloatCurveAsset> asset;
};

} // namespace rocks
