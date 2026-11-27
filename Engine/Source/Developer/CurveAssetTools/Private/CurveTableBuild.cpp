#include "CurveTableBuild.h"

#include <CurveAssets/CurveTableAsset.generated.h>
#include <Object/ObjectRegistry.h>

// 커브 표 임포터의 공용 쓰기다(Docs/Design/CurveAssets 5.1.1절).
namespace rocks::curve_import_detail {

void BeginTable(CurveTableAsset& staged, CurveInterp interp) noexcept {
    staged.EmptyTable();
    staged.mode = interp == CurveInterp::Cubic ? CurveTableMode::Tangent : CurveTableMode::Plain;
}

Result<void> AddTableRow(CurveTableAsset& staged, Name row, Span<const float> times,
                         Span<const float> values, CurveInterp interp) noexcept {
    if (staged.mode == CurveTableMode::Plain) {
        auto added = staged.TryAddPlainRow(row);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        PlainCurve& curve = *added.Value();
        curve.interp =
            interp == CurveInterp::Constant ? PlainCurveInterp::Constant : PlainCurveInterp::Linear;
        for (usize index = 0; index < times.Size(); ++index) {
            if (auto key = curve.TryAddKey(times[index], values[index]); !key) {
                return Result<void>::Failure(key.Error());
            }
        }

        return Result<void>::Success();
    }

    auto added = staged.TryAddTangentRow(row);
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    TangentCurve& curve = *added.Value();
    for (usize index = 0; index < times.Size(); ++index) {
        auto key = curve.TryAddKey(times[index], values[index]);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (auto set = curve.TrySetKeyInterp(key.Value(), CurveInterp::Cubic); !set) {
            return set;
        }
    }

    if (curve.KeyCount() > 0) {
        curve.AutoSetTangents();
    }

    return Result<void>::Success();
}

Result<void> PublishTable(AssetImportContext& context, TypeId outputType,
                          CurveTableAsset& staged) noexcept {
    auto made = context.Output({}, outputType);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    auto writable =
        context.Registry().ResolveForWrite<CurveTableAsset>(context.Access(), made.Value());
    if (!writable) {
        return Result<void>::Failure(writable.Error());
    }

    auto table = writable.Value().Get();
    if (!table) {
        return Result<void>::Failure(table.Error());
    }

    CurveTableAsset& output = *table.Value();
    output.EmptyTable();
    output.mode = staged.mode;
    output.plainRows = Move(staged.plainRows);
    output.tangentRows = Move(staged.tangentRows);
    return Result<void>::Success();
}

} // namespace rocks::curve_import_detail
