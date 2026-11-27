#include <AssetCooker/CookTargets.h>
#include <CurveAssetTools/CurveTableCook.h>
#include <CurveAssets/CurveTableAsset.generated.h>
#include <CurveAssets/CurveTableAsset.h>
#include <Object/ObjectRegistry.h>

// 커브 표 쿠킹 처리기다(Docs/Design/CurveAssets 5.3절).
namespace rocks {

namespace {

// 중복 키 제거의 허용오차(UE RemoveRedundantKeys(0.f))와 그것을 하는 최소 키 수(UE 조기 반환 `<
// 3`)다.
constexpr float CookRedundantKeyTolerance = 0.0f;
constexpr usize CookRedundantKeyMinimum = 3;

Result<void> CookFail(const CookContext& context, AssetDiagnosticCode code,
                      ErrorCode error) noexcept {
    if (context.failure != nullptr) {
        *context.failure = AssetDiagnostic{};
        context.failure->code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        context.failure->error = error;
        context.failure->asset = context.entry != nullptr ? context.entry->id : AssetId{};
    }

    return Result<void>::Failure(error);
}

Result<void> CookWarn(const CookContext& context, AssetDiagnosticCode code,
                      uint32 setting) noexcept {
    if (context.warnings == nullptr) {
        return Result<void>::Success();
    }

    const AssetId asset = context.entry != nullptr ? context.entry->id : AssetId{};
    return context.warnings->TryPushBack(
        CookDiagnostic{code, true, asset, {}, setting, ErrorCode::None});
}

// source 맵의 행을 깊이 복사해 키를 줄인 사본 맵을 만든다. reduce(curve)가 행 하나를 줄인다.
template <class Curve, class Reduce>
Result<void> ReduceRows(const OrderedMap<Name, Curve>& source, OrderedMap<Name, Curve>& out,
                        Reduce&& reduce) noexcept {
    ErrorCode failed = ErrorCode::None;
    source.ForEach([&](const Name& name, const Curve& curve) noexcept {
        if (failed != ErrorCode::None) {
            return;
        }

        Curve copy(out.GetAllocator());
        if (auto copied = copy.TryCopyFrom(curve); !copied) {
            failed = copied.Error();
            return;
        }

        if (auto reduced = reduce(copy); !reduced) {
            failed = reduced.Error();
            return;
        }

        if (auto inserted = out.Insert(name, Move(copy)); !inserted) {
            failed = inserted.Error();
        }
    });
    return failed == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(failed);
}

// 쓰기용 사본과 되돌릴 원래 맵이다.
struct CookedRows {
    explicit CookedRows(Allocator& allocator) noexcept
        : plain(allocator), tangent(allocator), savedPlain(allocator), savedTangent(allocator) {
    }

    OrderedMap<Name, PlainCurve> plain;
    OrderedMap<Name, TangentCurve> tangent;
    OrderedMap<Name, PlainCurve> savedPlain;
    OrderedMap<Name, TangentCurve> savedTangent;
};

template <class Fn>
Result<void> WithTable(CookContext& context, ObjectHandle asset, Fn&& fn) noexcept {
    auto borrowed = context.registry->ResolveForWrite<CurveTableAsset>(context.access, asset);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return fn(*value.Value());
}

} // namespace

Result<void> CurveTableCookProcessor::Cook(CookContext& context) const noexcept {
    ObjectRegistry& registry = *context.registry;
    const ObjectAccessContext& access = context.access;
    auto asset = registry.FindChild(access, context.package, "");
    if (!asset) {
        return CookFail(context, AssetDiagnosticCode::None, asset.Error());
    }

    auto type = registry.AcquireType(access, asset.Value());
    if (!type) {
        return CookFail(context, AssetDiagnosticCode::None, type.Error());
    }

    if (type.Value().Id() != TypeIdOf<CurveTableAsset>()) {
        return CookFail(context, AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    // 1·2. 사본을 만들고(에디터 객체 맵의 allocator — 되돌린 뒤 객체가 쿠킹 할당기를 쥐지 않게)
    // 이동으로 바꿔 넣는다.
    Allocator* objectAllocator = nullptr;
    usize dropped = 0;
    // 사본은 객체 allocator로 만들어야 하므로 그 allocator를 먼저 읽는다(읽기 borrow).
    {
        auto borrowed = registry.Resolve<CurveTableAsset>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        objectAllocator = &value.Value()->plainRows.GetAllocator();
    }

    CookedRows cooked(*objectAllocator);
    auto swapped = WithTable(context, asset.Value(), [&](CurveTableAsset& table) noexcept {
        const CurveTableMode mode = table.mode;
        dropped = (mode == CurveTableMode::Plain ? 0 : table.plainRows.Count()) +
                  (mode == CurveTableMode::Tangent ? 0 : table.tangentRows.Count());
        if (mode == CurveTableMode::Plain) {
            auto reduced =
                ReduceRows(table.plainRows, cooked.plain, [](PlainCurve& curve) noexcept {
                    return curve.KeyCount() >= CookRedundantKeyMinimum
                               ? curve.TryRemoveRedundantKeys(CookRedundantKeyTolerance)
                               : Result<void>::Success();
                });
            if (!reduced) {
                return reduced;
            }
        } else if (mode == CurveTableMode::Tangent) {
            auto reduced =
                ReduceRows(table.tangentRows, cooked.tangent, [](TangentCurve& curve) noexcept {
                    if (curve.KeyCount() < CookRedundantKeyMinimum) {
                        return Result<void>::Success();
                    }

                    if (auto removed = curve.TryRemoveRedundantKeys(CookRedundantKeyTolerance);
                        !removed) {
                        return removed;
                    }

                    curve.AutoSetTangents();
                    return Result<void>::Success();
                });
            if (!reduced) {
                return reduced;
            }
        }

        cooked.savedPlain = Move(table.plainRows);
        cooked.savedTangent = Move(table.tangentRows);
        table.plainRows = Move(cooked.plain);
        table.tangentRows = Move(cooked.tangent);
        return Result<void>::Success();
    });
    if (!swapped) {
        return CookFail(context, AssetDiagnosticCode::None, swapped.Error());
    }

    // 경고(입력만으로 정해진다 — 다시 쿠킹해도 같다).
    ErrorCode writeError = ErrorCode::None;
    if (dropped != 0) {
        if (auto warned = CookWarn(context, AssetDiagnosticCode::CurveTableUnusedRows,
                                   static_cast<uint32>(dropped));
            !warned) {
            writeError = warned.Error();
        }
    }

    // 3. 쓰고, 성공·실패 모두 원래 맵을 되돌린다.
    if (writeError == ErrorCode::None) {
        // bulk 정렬은 프로파일 값이다(커브 표에는 bulk가 없다 — 0 = 최소 정렬 없음).
        if (auto written = WriteCookedPackage(context, 0); !written) {
            writeError = written.Error();
        }
    }

    Result<void> restored = WithTable(context, asset.Value(), [&](CurveTableAsset& table) noexcept {
        table.plainRows = Move(cooked.savedPlain);
        table.tangentRows = Move(cooked.savedTangent);
        return Result<void>::Success();
    });
    if (writeError != ErrorCode::None) {
        return Result<void>::Failure(writeError);
    }

    if (!restored) {
        return CookFail(context, AssetDiagnosticCode::None, restored.Error());
    }

    return Result<void>::Success();
}

Result<void> RegisterCurveTableCookProcessor(void* state, CookProcessorTable& table) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return table.Register(TypeIdOf<CurveTableAsset>(),
                          *static_cast<const CurveTableCookProcessor*>(state));
}

} // namespace rocks
