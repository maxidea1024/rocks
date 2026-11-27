#pragma once

#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <CurveAssets/CurveTableAsset.h>
#include <Meta/Annotations.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>

// 커브 표의 행 하나를 가리킨다(Docs/Design/CurveAssets 3.3절, UE FCurveTableRowHandle 대응).
//
// 참조 struct다(ObjectRef — Hard): 리플렉션 클래스의 직접 필드나 StructRefArray 원소에만 둔다(설계
// CAD7).
namespace rocks {

class ObjectRegistry;

// table은 ObjectRegistry::SetReference로 바꾼다.
//
// 사용 예(게임 수치 — 레벨에 따른 피해량):
//   auto damage = holder->damage.TryEvaluate(registry, access, static_cast<float>(level));
//   if (!damage) { ... }   // 표 없음·행 없음 NotFound
ROCKS_STRUCT()
struct ROCKS_CURVEASSETS_API CurveTableRowRef {
    ROCKS_GENERATED_BODY()

    // 표가 null이고 행이 None이다(UE IsNull).
    bool IsNull() const noexcept {
        return table.Handle().IsNull() && row.IsNone();
    }

    // 표를 풀어 그 행을 평가한다(UE Eval·EvaluateCurveTableRow). 오류: 표가 null이면 NotFound, 행이
    // 없으면 NotFound, 표가 풀리지 않으면 Resolve의 오류(파괴 요청 Destroyed, 다른 스레드
    // WrongThread). 로그를 남기지 않는다.
    Result<float> TryEvaluate(ObjectRegistry& registry, const ObjectAccessContext& access,
                              float time) const noexcept;

    ROCKS_PROPERTY() ObjectRef<CurveTableAsset> table;
    ROCKS_PROPERTY() Name row;
};

} // namespace rocks
