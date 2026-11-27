#include "CurveImportSupport.h"

#include <Core/Assert.h>
#include <Core/Text/Csv.h>
#include <CurveAssetTools/CurveCsvImporter.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/CurveAsset.generated.h>
#include <CurveAssets/FloatCurveAsset.generated.h>
#include <CurveAssets/VectorCurveAsset.generated.h>
#include <Object/ObjectRegistry.h>

// CSV 단일 커브 임포터다(Docs/Design/CurveAssets 5.1.4절, UE UCurveBase::CreateCurveFromCSVString —
// CurveBase.cpp:131-191).
namespace rocks {

namespace {

constexpr Guid FloatCurveCsvImporterId{0x4a91c7e3b25d4f08ull, 0xa3e6f1c08d2b5e74ull};
constexpr Guid VectorCurveCsvImporterId{0x9e05b2d8c6f14a3bull, 0x81d7e4a2f0c93b65ull};
constexpr Guid ColorCurveCsvImporterId{0x15f8a3c9e7d24b60ull, 0xbc42d9e1a07f6385ull};

// 행의 첫 칸(시간)과 최소 칸 수다(시간 + 값 하나 — UE "less than 2 cells").
constexpr usize TimeCell = 0;
constexpr usize MinimumCells = 2;
// 출력 종류의 채널 수와 그 최댓값이다(스칼라 1, 벡터 X·Y·Z, 색 R·G·B·A).
constexpr usize FloatChannels = 1;
constexpr usize VectorChannels = 3;
constexpr usize ColorChannels = 4;
constexpr usize MaxChannels = ColorChannels;

// 출력의 채널 키를 staged로 바꾼다(UE ResetCurve 뒤 AddKey). 기본값·외삽은 대상 것을 사본에 옮겨
// 지킨다. 이동만 하므로 실패하지 않는다(대상이 반쯤 바뀌는 일이 없다).
void WriteChannels(CurveAsset& asset, Span<TangentCurve> staged) noexcept {
    ROCKS_ASSERT(asset.ChannelCount() == staged.Size());
    for (usize channel = 0; channel < staged.Size(); ++channel) {
        TangentCurve& target = asset.MutableChannel(channel);
        staged[channel].defaultValue = target.defaultValue;
        staged[channel].preExtrapolation = target.preExtrapolation;
        staged[channel].postExtrapolation = target.postExtrapolation;
        target = Move(staged[channel]);
    }
}

} // namespace

CurveCsvImporter::CurveCsvImporter(CurveCsvOutput output) noexcept : output_(output) {
    info_.version = CurveCsvImporterVersion;
    if (output == CurveCsvOutput::Float) {
        info_.id = FloatCurveCsvImporterId;
        info_.name = FloatCurveCsvImporterName;
        info_.outputType = TypeIdOf<FloatCurveAsset>();
    } else if (output == CurveCsvOutput::Vector) {
        info_.id = VectorCurveCsvImporterId;
        info_.name = VectorCurveCsvImporterName;
        info_.outputType = TypeIdOf<VectorCurveAsset>();
    } else {
        info_.id = ColorCurveCsvImporterId;
        info_.name = ColorCurveCsvImporterName;
        info_.outputType = TypeIdOf<ColorCurveAsset>();
    }
}

Result<void> CurveCsvImporter::Import(AssetImportContext& context) noexcept {
    using namespace curve_import_detail;
    ObjectRegistry& registry = context.Registry();
    Allocator& allocator = registry.ObjectAllocator();
    String text(allocator);
    if (auto read = ReadSourceText(context, text); !read) {
        return read;
    }

    Array<CsvRow> rows(allocator);
    if (auto parsed = TryParseCsv(text.View(), rows); !parsed) {
        return parsed;
    }

    // 행이 없으면 실패한다(UE "No data." — 키 그대로).
    if (rows.Size() == 0) {
        return FailFormat(context, 0);
    }

    const usize channelCount =
        output_ == CurveCsvOutput::Float
            ? FloatChannels
            : (output_ == CurveCsvOutput::Vector ? VectorChannels : ColorChannels);
    TangentCurve staged[MaxChannels] = {TangentCurve(allocator), TangentCurve(allocator),
                                        TangentCurve(allocator), TangentCurve(allocator)};
    for (const CsvRow& row : rows) {
        if (row.cells.Size() < MinimumCells) {
            if (auto warned = context.Warn(AssetDiagnosticCode::CurveImportRowSkipped, row.line);
                !warned) {
                return warned;
            }

            continue;
        }

        auto time = ParseNumberCell(row.cells[TimeCell].View());
        if (!time) {
            return FailFormat(context, row.line);
        }

        // 있는 채널에만 키를 넣는다(남는 칸은 버린다 — UE와 같다). 칸 수가 맞지 않으면 경고.
        for (usize cell = 1; cell < row.cells.Size() && cell <= channelCount; ++cell) {
            auto value = ParseNumberCell(row.cells[cell].View());
            if (!value) {
                return FailFormat(context, row.line);
            }

            TangentCurve& curve = staged[cell - 1];
            auto key = curve.TryAddKey(time.Value(), value.Value());
            if (!key) {
                return Result<void>::Failure(key.Error());
            }

            if (auto set = curve.TrySetKeyInterp(key.Value(), CurveInterp::Linear); !set) {
                return set;
            }
        }

        if (row.cells.Size() != channelCount + 1) {
            if (auto warned = context.Warn(AssetDiagnosticCode::CurveImportRowSkipped, row.line);
                !warned) {
                return warned;
            }
        }
    }

    for (usize channel = 0; channel < channelCount; ++channel) {
        if (staged[channel].KeyCount() > 0) {
            staged[channel].AutoSetTangents();
        }
    }

    auto made = context.Output({}, info_.outputType);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    auto writable = registry.ResolveForWrite<CurveAsset>(context.Access(), made.Value());
    if (!writable) {
        return Result<void>::Failure(writable.Error());
    }

    auto asset = writable.Value().Get();
    if (!asset) {
        return Result<void>::Failure(asset.Error());
    }

    WriteChannels(*asset.Value(), Span<TangentCurve>(staged, channelCount));
    return Result<void>::Success();
}

} // namespace rocks
