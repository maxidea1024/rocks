#pragma once

#include <Curves/CurveTypes.h>
#include <Meta/Annotations.h>

// 커브 표 임포트 옵션이다(Docs/Design/CurveAssets 5.1절 — curve_table_csv·curve_table_json, UE
// UCSVImportFactory의 보간 모드 선택·UCurveTable::ImportCurveInterpMode). ImportRecord가 보존해
// 재임포트가 같은 값을 쓴다(에셋에는 두지 않는다). 런타임 모듈에 두는 것은 기록의 옵션 값이
// 리플렉션 타입이라서다 (SoundClipImportOptions와 같다).
namespace rocks {

// interp가 Cubic이면 Tangent 표(키마다 Cubic·자동 접선), Linear·Constant면 Plain 표(커브 하나에
// 보간 하나)다(UE CreateTableFromCSVString — CurveTable.cpp:676).
//
// 사용 예(명령행):
//   asset-import --importer=curve_table_csv --interp=Cubic Levels.csv Data/LevelTable
ROCKS_STRUCT()
struct CurveTableImportOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() CurveInterp interp = CurveInterp::Linear;
};

} // namespace rocks
