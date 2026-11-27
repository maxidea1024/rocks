#include "CurveImportSupport.h"
#include "CurveTableBuild.h"

#include <Core/Text/Csv.h>
#include <CurveAssetTools/CurveTableCsvImporter.h>
#include <CurveAssets/CurveTableAsset.generated.h>
#include <CurveAssets/CurveTableImportOptions.generated.h>
#include <Object/ObjectRegistry.h>

// CSV 커브 표 임포터다(Docs/Design/CurveAssets 5.1.2절, UE UCurveTable::CreateTableFromCSVString —
// CurveTable.cpp:656-760).
namespace rocks {

namespace {

constexpr Guid CurveTableCsvImporterId{0x7c3e1a90d4b54f62ull, 0x8b21e6f09a3d4c57ull};

} // namespace

CurveTableCsvImporter::CurveTableCsvImporter() noexcept {
    info_.id = CurveTableCsvImporterId;
    info_.name = CurveTableCsvImporterName;
    info_.version = CurveTableCsvImporterVersion;
    info_.outputType = TypeIdOf<CurveTableAsset>();
    info_.optionsType = TypeIdOf<CurveTableImportOptions>();
}

Result<void> CurveTableCsvImporter::Import(AssetImportContext& context) noexcept {
    using namespace curve_import_detail;
    CurveTableImportOptions options;
    if (const void* given = context.Options(); given != nullptr) {
        options = *static_cast<const CurveTableImportOptions*>(given);
    }

    if (!IsKnownInterp(options.interp)) {
        return context.Fail(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                            0);
    }

    ObjectRegistry& registry = context.Registry();
    NamePool* names = registry.Names();
    if (names == nullptr) {
        return context.Fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument, 0);
    }

    Allocator& allocator = registry.ObjectAllocator();
    String text(allocator);
    if (auto read = ReadSourceText(context, text); !read) {
        return read;
    }

    Array<CsvRow> rows(allocator);
    if (auto parsed = TryParseCsv(text.View(), rows); !parsed) {
        return parsed;
    }

    // 시간 축 행과 값 행 하나 이상이 있어야 한다(UE "Too few rows.").
    if (rows.Size() <= 1) {
        return FailFormat(context, rows.Size() == 1 ? rows[0].line : 0);
    }

    // 첫 행: 0열은 무시하고 1열부터 시간이다(UE GetCurveValues — 칸이 2개 미만이면 시간 없음).
    Array<float> times(allocator);
    const CsvRow& header = rows[0];
    for (usize cell = 1; cell < header.cells.Size(); ++cell) {
        auto time = ParseNumberCell(header.cells[cell].View());
        if (!time) {
            return FailFormat(context, header.line);
        }

        for (const float earlier : times) {
            if (earlier == time.Value()) {
                // 같은 시간 열(UE FindDuplicateXValues).
                return FailFormat(context, header.line);
            }
        }

        if (auto pushed = times.TryPushBack(time.Value()); !pushed) {
            return pushed;
        }
    }

    CurveTableAsset staged(allocator);
    BeginTable(staged, options.interp);
    Array<float> values(allocator);
    for (usize index = 1; index < rows.Size(); ++index) {
        const CsvRow& row = rows[index];
        auto name = MakeRowName(*names, row.cells[0].View(), allocator);
        if (!name) {
            return FailRowName(context, name.Error(), row.line);
        }

        // 이름 없음·이름 중복·열 수 불일치는 그 행만 건너뛴다(UE와 같다).
        const usize valueCount = row.cells.Size() >= 2 ? row.cells.Size() - 1 : 0;
        if (name.Value().IsNone() || staged.FindRow(name.Value()) != nullptr ||
            valueCount != times.Size()) {
            if (auto warned = context.Warn(AssetDiagnosticCode::CurveImportRowSkipped, row.line);
                !warned) {
                return warned;
            }

            continue;
        }

        values.Clear();
        for (usize cell = 1; cell < row.cells.Size(); ++cell) {
            auto value = ParseNumberCell(row.cells[cell].View());
            if (!value) {
                return FailFormat(context, row.line);
            }

            if (auto pushed = values.TryPushBack(value.Value()); !pushed) {
                return pushed;
            }
        }

        if (auto added =
                AddTableRow(staged, name.Value(), times.View(), values.View(), options.interp);
            !added) {
            return added;
        }
    }

    return PublishTable(context, info_.outputType, staged);
}

} // namespace rocks
