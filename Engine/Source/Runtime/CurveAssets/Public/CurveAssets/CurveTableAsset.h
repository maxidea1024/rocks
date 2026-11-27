#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <CurveAssets/CurveTableMode.h>
#include <Curves/PlainCurve.h>
#include <Curves/TangentCurve.h>

// 이름 붙은 스칼라 커브의 표다(Docs/Design/CurveAssets 3.2절, UE UCurveTable 대응).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다. 조회·평가는 할당하지 않는다.
namespace rocks {

// 모드가 쓸 맵을 정한다(Plain → plainRows, Tangent → tangentRows, Empty → 없음). 모드가 쓰지 않는
// 맵의 행은 조회·평가에서 없는 것으로 친다(경로 쓰기·Inspector가 반대쪽에 써도 에셋은 열린다 —
// 쿠킹이 비운다). 편집 함수(추가·지우기·이름 바꾸기·비우기)는 모드가 쓰지 않는 맵의 저장소를
// 돌려준다. 행 순서는 삽입 순서다(지운 뒤 다시 넣거나 이름을 바꾸면 끝).
//
// 사용 예(게임 수치 — 레벨 3의 체력):
//   auto health = table->TryEvaluateRow(healthRow, 3.0f);
//   if (!health) { return Result<void>::Failure(health.Error()); }   // 행 없음 NotFound
ROCKS_CLASS()
class ROCKS_CURVEASSETS_API CurveTableAsset final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit CurveTableAsset(Allocator& allocator) noexcept
        : plainRows(allocator), tangentRows(allocator) {
    }

    // ---- 조회(로그 없음 — 오류 판단은 호출자) ----

    // 모드가 쓰는 맵의 행이다(커브 공통 칸 — 기본값·외삽). 없으면 null(UE FindCurveUnchecked).
    const ScalarCurveBase* FindRow(Name row) const noexcept;

    // 모드가 Plain·Tangent일 때만 그 행이다. 다른 모드면 null(UE FindSimpleCurve·FindRichCurve는
    // Error 로그 뒤 null).
    const PlainCurve* FindPlainRow(Name row) const noexcept;
    const TangentCurve* FindTangentRow(Name row) const noexcept;

    // 모드에 맞는 행의 Evaluate(time)다(fallback 0). 실패: 행이 없으면(None 이름 포함) NotFound.
    Result<float> TryEvaluateRow(Name row, float time) const noexcept;

    // 모드가 쓰는 맵의 행 수다.
    usize RowCount() const noexcept;

    // 모드가 쓰는 맵의 행 이름을 삽입 순서로 visitor(Name)에 넘긴다. visitor 안에서 표를 바꾸지
    // 않는다.
    template <class Visitor>
    void ForEachRowName(Visitor&& visitor) const noexcept {
        if (mode == CurveTableMode::Plain) {
            plainRows.ForEach(
                [&visitor](const Name& name, const PlainCurve&) noexcept { visitor(name); });
        } else if (mode == CurveTableMode::Tangent) {
            tangentRows.ForEach(
                [&visitor](const Name& name, const TangentCurve&) noexcept { visitor(name); });
        }
    }

    // ---- 편집 ----

    // row 행을 빈 커브로 만들어 그 포인터를 돌려준다(UE AddRichCurve·AddSimpleCurve). 모드가
    // Empty면 그 모드로 정한다. 같은 이름 행이 있으면 빈 커브로 바꾼다(자리 유지). 포인터는 다음 표
    // 편집까지 유효하다. 오류: 다른 모드의 표 TypeMismatch(UE check), None 이름 InvalidArgument,
    // 할당 실패 OutOfMemory(표 그대로).
    Result<PlainCurve*> TryAddPlainRow(Name row) noexcept;
    Result<TangentCurve*> TryAddTangentRow(Name row) noexcept;

    // row 행을 지운다. 있었으면 참이다(UE RemoveRow·DeleteRow). 모드는 그대로다.
    bool RemoveRow(Name row) noexcept;

    // 행 이름을 바꾼다(설계 3.2.3절 판정 순서): 같으면 성공(아무 일 없음), from이 없으면 NotFound,
    // to가 None이면 InvalidArgument, to가 이미 있으면 성공(아무 일 없음 — UE와 같다), 그 밖은
    // 지우고 새 이름으로 끝에 넣는다. 오류: 위와 할당 실패(OutOfMemory — 표 그대로).
    Result<void> TryRenameRow(Name from, Name to) noexcept;

    // 모든 행을 지우고 모드를 Empty로 한다(UE EmptyTable — 저장소도 돌려준다).
    void EmptyTable() noexcept;

private:
    // 모드가 쓰지 않는 맵의 행과 저장소를 버린다(할당 없음).
    void ReleaseUnusedRows() noexcept;

public:
    ROCKS_PROPERTY() CurveTableMode mode = CurveTableMode::Empty;
    // 모드가 쓰는 맵만 행을 가진다(설계 3.2.1절 — 다른 맵은 무시한다).
    ROCKS_PROPERTY() OrderedMap<Name, PlainCurve> plainRows;
    ROCKS_PROPERTY() OrderedMap<Name, TangentCurve> tangentRows;
};

} // namespace rocks
