#include <CurveAssetTools/CurveAssetImporterSet.h>

// 커브 에셋 임포터 묶음 등록이다(Docs/Design/CurveAssets 5.1절).
namespace rocks {

namespace {

constexpr usize CurveImporterCount = 5;

} // namespace

Result<void> RegisterCurveAssetImporters(void* state, AssetImporterTable& table,
                                         Array<AssetImporterRegistration>& tokens) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& set = *static_cast<CurveAssetImporterSet*>(state);
    if (auto reserved = tokens.TryReserve(tokens.Size() + CurveImporterCount); !reserved) {
        return reserved;
    }

    AssetImporter* importers[CurveImporterCount] = {&set.tableCsv, &set.tableJson, &set.floatCsv,
                                                    &set.vectorCsv, &set.colorCsv};
    // 하나라도 실패하면 이번에 등록한 것을 푼다(토큰 소멸이 해제한다 — tokens 그대로).
    Array<AssetImporterRegistration> staged(tokens.GetAllocator());
    if (auto reserved = staged.TryReserve(CurveImporterCount); !reserved) {
        return reserved;
    }

    for (AssetImporter* importer : importers) {
        auto registered = table.Register(*importer);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        (void)staged.TryPushBack(Move(registered.Value()));
    }

    for (AssetImporterRegistration& registration : staged) {
        (void)tokens.TryPushBack(Move(registration));
    }

    return Result<void>::Success();
}

} // namespace rocks
