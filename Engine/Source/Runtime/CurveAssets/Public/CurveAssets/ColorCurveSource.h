#pragma once

#include <Core/Math/Color.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <CurveAssets/ColorCurveAsset.h>
#include <CurveAssets/ColorCurveValue.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>

// 인라인 색 커브 또는 색 커브 에셋이다(Docs/Design/CurveAssets 3.1절, UE FRuntimeCurveLinearColor
// 대응). 참조 struct다 — 둘 수 있는 자리는 FloatCurveSource와 같다.
namespace rocks {

class ObjectRegistry;

// asset이 null이 아니면 그 에셋의 조정 적용 색, null이면 r·g·b·a의 조정 없는 색(알파 키가 없으면 A
// = 1)이다(UE와 같다 — 인라인에는 조정 필드가 없다).
//
// 사용 예:
//   auto tint = holder->tint.TryEvaluate(registry, access, age);
ROCKS_STRUCT()
struct ROCKS_CURVEASSETS_API ColorCurveSource {
    ROCKS_GENERATED_BODY()

    explicit ColorCurveSource(Allocator& allocator) noexcept
        : r(allocator), g(allocator), b(allocator), a(allocator) {
    }

    bool HasAsset() const noexcept {
        return !asset.Handle().IsNull();
    }

    // 인라인 커브의 조정 없는 색이다(참조는 보지 않는다).
    LinearColor EvaluateInline(float time) const noexcept {
        return EvaluateUnadjustedColorCurve(r, g, b, a, time);
    }

    // 쓰일 색이다(에셋이면 ColorCurveAsset::Evaluate — 조정 적용). 오류는
    // FloatCurveSource::TryEvaluate와 같다.
    Result<LinearColor> TryEvaluate(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    float time) const noexcept;

    // 쓰일 커브를 out에 깊이 복사한다: 에셋이면 커브 넷·조정 일곱·adjusted = 참, 인라인이면 커브
    // 넷· 기본 조정·adjusted = 거짓. 오류는 FloatCurveSource::TryFlattenInto와 같다(실패하면 out
    // 그대로).
    Result<void> TryFlattenInto(ObjectRegistry& registry, const ObjectAccessContext& access,
                                ColorCurveValue& out) const noexcept;

    ROCKS_PROPERTY() TangentCurve r;
    ROCKS_PROPERTY() TangentCurve g;
    ROCKS_PROPERTY() TangentCurve b;
    ROCKS_PROPERTY() TangentCurve a;
    ROCKS_PROPERTY() ObjectRef<ColorCurveAsset> asset;
};

} // namespace rocks
