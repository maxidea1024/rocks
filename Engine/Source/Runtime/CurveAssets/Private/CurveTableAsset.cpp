#include <CurveAssets/CurveTableAsset.h>

// 커브 표의 조회·편집이다(Docs/Design/CurveAssets 3.2절, UE UCurveTable — CurveTable.cpp:527-620).
namespace rocks {

namespace {

// map에서 row 행을 빈 커브로 만들고 그 포인터를 돌려준다(같은 이름이면 자리 유지).
template <class Curve>
Result<Curve*> AddEmptyRow(OrderedMap<Name, Curve>& map, Name row) noexcept {
    Curve empty(map.GetAllocator());
    auto placed = map.InsertOrAssign(row, Move(empty));
    if (!placed) {
        return Result<Curve*>::Failure(placed.Error());
    }

    return Result<Curve*>::Success(map.Find(row));
}

// map의 from 행을 to로 옮긴다(끝에 넣는다). 넣기가 실패하면 값을 되돌리고 표는 그대로다.
template <class Curve>
Result<void> MoveRow(OrderedMap<Name, Curve>& map, Name from, Name to) noexcept {
    Curve* existing = map.Find(from);
    Curve taken = Move(*existing);
    auto inserted = map.Insert(to, Move(taken));
    if (!inserted) {
        // 실패한 Insert는 입력을 옮기지 않고 Map도 바꾸지 않는다 — 포인터가 그대로 유효하다.
        *existing = Move(taken);
        return Result<void>::Failure(inserted.Error());
    }

    (void)map.Erase(from);
    return Result<void>::Success();
}

} // namespace

void CurveTableAsset::ReleaseUnusedRows() noexcept {
    if (mode != CurveTableMode::Plain) {
        plainRows.Reset();
    }

    if (mode != CurveTableMode::Tangent) {
        tangentRows.Reset();
    }
}

const ScalarCurveBase* CurveTableAsset::FindRow(Name row) const noexcept {
    if (mode == CurveTableMode::Plain) {
        return plainRows.Find(row);
    }

    if (mode == CurveTableMode::Tangent) {
        return tangentRows.Find(row);
    }

    return nullptr;
}

const PlainCurve* CurveTableAsset::FindPlainRow(Name row) const noexcept {
    return mode == CurveTableMode::Plain ? plainRows.Find(row) : nullptr;
}

const TangentCurve* CurveTableAsset::FindTangentRow(Name row) const noexcept {
    return mode == CurveTableMode::Tangent ? tangentRows.Find(row) : nullptr;
}

Result<float> CurveTableAsset::TryEvaluateRow(Name row, float time) const noexcept {
    if (const PlainCurve* plain = FindPlainRow(row)) {
        return Result<float>::Success(plain->Evaluate(time));
    }

    if (const TangentCurve* tangent = FindTangentRow(row)) {
        return Result<float>::Success(tangent->Evaluate(time));
    }

    return Result<float>::Failure(ErrorCode::NotFound);
}

usize CurveTableAsset::RowCount() const noexcept {
    if (mode == CurveTableMode::Plain) {
        return plainRows.Count();
    }

    return mode == CurveTableMode::Tangent ? tangentRows.Count() : 0;
}

Result<PlainCurve*> CurveTableAsset::TryAddPlainRow(Name row) noexcept {
    if (mode == CurveTableMode::Tangent) {
        return Result<PlainCurve*>::Failure(ErrorCode::TypeMismatch);
    }

    if (row.IsNone()) {
        return Result<PlainCurve*>::Failure(ErrorCode::InvalidArgument);
    }

    // Empty 표의 맵에 남은 행(경로 쓰기가 쓴 것 — 보이지 않던 행)은 모드를 정하기 전에 비운다.
    if (mode == CurveTableMode::Empty) {
        plainRows.Clear();
    }

    auto added = AddEmptyRow(plainRows, row);
    if (added) {
        mode = CurveTableMode::Plain;
        ReleaseUnusedRows();
    }

    return added;
}

Result<TangentCurve*> CurveTableAsset::TryAddTangentRow(Name row) noexcept {
    if (mode == CurveTableMode::Plain) {
        return Result<TangentCurve*>::Failure(ErrorCode::TypeMismatch);
    }

    if (row.IsNone()) {
        return Result<TangentCurve*>::Failure(ErrorCode::InvalidArgument);
    }

    if (mode == CurveTableMode::Empty) {
        tangentRows.Clear();
    }

    auto added = AddEmptyRow(tangentRows, row);
    if (added) {
        mode = CurveTableMode::Tangent;
        ReleaseUnusedRows();
    }

    return added;
}

bool CurveTableAsset::RemoveRow(Name row) noexcept {
    ReleaseUnusedRows();
    if (mode == CurveTableMode::Plain) {
        return plainRows.Erase(row);
    }

    return mode == CurveTableMode::Tangent && tangentRows.Erase(row);
}

Result<void> CurveTableAsset::TryRenameRow(Name from, Name to) noexcept {
    ReleaseUnusedRows();
    if (from == to) {
        return Result<void>::Success();
    }

    if (FindRow(from) == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (to.IsNone()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (FindRow(to) != nullptr) {
        return Result<void>::Success();
    }

    return mode == CurveTableMode::Plain ? MoveRow(plainRows, from, to)
                                         : MoveRow(tangentRows, from, to);
}

void CurveTableAsset::EmptyTable() noexcept {
    mode = CurveTableMode::Empty;
    ReleaseUnusedRows();
}

} // namespace rocks
