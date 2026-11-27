#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <Core/Containers/Array.h>
#include <Core/Text/Name.h>
#include <CurveAssets/CurveTableAsset.h>
#include <Curves/CurveTypes.h>

// 커브 표 임포터(CSV·JSON)의 공용 쓰기다(Docs/Design/CurveAssets 5.1.1절). 사본 표에 행을 다 만든
// 뒤 출력 에셋으로 옮긴다(형식 오류로 실패하면 출력에 아무것도 쓰지 않는다).
namespace rocks::curve_import_detail {

// interp로 모드를 정한 빈 사본 표다(Cubic → Tangent, 그 밖 → Plain — UE CurveTable.cpp:676).
void BeginTable(CurveTableAsset& staged, CurveInterp interp) noexcept;

// staged에 row 행을 (times[i], values[i]) 키로 더한다. Plain이면 커브 보간을 interp로, Tangent면
// 키마다 Cubic이고 끝에 자동 접선을 계산한다(UE SetKeyInterpMode가 키마다 자동 접선 — 마지막 결과가
// 같다). 오류: 할당 실패, 비유한 시간(호출자가 미리 거른다).
Result<void> AddTableRow(CurveTableAsset& staged, Name row, Span<const float> times,
                         Span<const float> values, CurveInterp interp) noexcept;

// 출력 에셋을 만들어(재임포트면 기존) staged의 모드·행을 옮긴다. 오류: Output·Resolve 오류.
Result<void> PublishTable(AssetImportContext& context, TypeId outputType,
                          CurveTableAsset& staged) noexcept;

} // namespace rocks::curve_import_detail
