#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <Core/Containers/Array.h>
#include <CurveAssetTools/CurveCsvImporter.h>
#include <CurveAssetTools/CurveTableCsvImporter.h>
#include <CurveAssetTools/CurveTableJsonImporter.h>

// 커브 에셋 임포터 다섯의 묶음이다(Docs/Design/CurveAssets 5.1절). 제품 호스트(명령행
// asset-import)가 한 칸으로 등록한다.
//
// 사용 예:
//   CurveAssetImporterSet curves;
//   if (auto registered = RegisterCurveAssetImporters(&curves, table, tokens); !registered) { ... }
namespace rocks {

struct CurveAssetImporterSet {
    CurveTableCsvImporter tableCsv;
    CurveTableJsonImporter tableJson;
    CurveCsvImporter floatCsv{CurveCsvOutput::Float};
    CurveCsvImporter vectorCsv{CurveCsvOutput::Vector};
    CurveCsvImporter colorCsv{CurveCsvOutput::Color};
};

// state(CurveAssetImporterSet*)의 다섯을 table에 등록하고 토큰을 tokens에 둔다(commandlet_host::
// AssetImporterSetup 모양). 오류: null state InvalidArgument, 등록 오류(커브 에셋 타입이 등록되지
// 않았으면 NotFound), 할당 실패. 실패하면 이번에 등록한 것은 풀린다(tokens 그대로).
ROCKS_CURVEASSETTOOLS_API Result<void>
RegisterCurveAssetImporters(void* state, AssetImporterTable& table,
                            Array<AssetImporterRegistration>& tokens) noexcept;

} // namespace rocks
