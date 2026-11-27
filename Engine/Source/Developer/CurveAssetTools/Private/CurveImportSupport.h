#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Curves/CurveTypes.h>

// 커브 임포터의 공용 도우미다(Docs/Design/CurveAssets 5.1.1절).
namespace rocks::curve_import_detail {

// 행 이름에서 빼는 문자다(UE INVALID_NAME_CHARACTERS — UCurveTable::MakeValidName,
// CurveTable.cpp:79-100).
inline constexpr StringView CurveTableInvalidNameCharacters = "\"' ,\n\r\t";

// 원본 byte를 UTF-8 텍스트로 만든다(UE LoadFileToString처럼 BOM을 본다): UTF-8 BOM은 떼고, UTF-16
// LE·BE BOM이면 UTF-8로 바꾸고, BOM이 없으면 UTF-8로 검증한다. 오류: 올바르지 않은 UTF-8·UTF-16
// CorruptData, 할당 실패. 실패하면 out은 그대로다.
Result<void> DecodeImportText(Span<const uint8> bytes, String& out) noexcept;

// 숫자 칸이다: 앞뒤 ASCII 공백·탭을 떼고 칸 전체를 double로 읽어 float로 바꾼다(UE Windows Atof =
// (float)_tstof — double을 거친다). 오류: 숫자가 아니거나 비유한이면 InvalidArgument(UE Atof는
// 0으로 읽는다 — 설계 7.2절 5).
Result<float> ParseNumberCell(StringView cell) noexcept;

// JSON 문자열 값의 숫자다(UE FJsonValueString::TryGetNumber — FString::IsNumeric이 참일 때만 Atod):
// [+|-] 뒤에 숫자와 '.' 하나까지만(지수·공백 없음)이고, 숫자가 하나도 없으면("."·"-") 0이다. 오류:
// 형식이 아니거나 float로 비유한이면 InvalidArgument.
Result<float> ParseJsonNumberString(StringView text) noexcept;

// 행 이름 intern 실패를 돌려준다: 할당·용량 실패는 그대로(원본 탓이 아니다), 그 밖은 InvalidSource
// 진단(detail = 위치).
Result<void> FailRowName(AssetImportContext& context, ErrorCode error, uint64 position) noexcept;

// 행 이름을 Name으로 만든다(금지 문자를 뺀다 — 비면 None). 오류: intern 실패(잘못된 UTF-8·할당).
Result<Name> MakeRowName(NamePool& names, StringView raw, Allocator& allocator) noexcept;

// 형식 오류를 진단과 함께 돌려준다(InvalidSource, InvalidArgument, detail = 위치).
inline Result<void> FailFormat(AssetImportContext& context, uint64 position) noexcept {
    return context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, position);
}

// 원본 하나를 텍스트로 읽는다(디코드 실패는 InvalidSource 진단 — CorruptData, 할당 실패는 진단 없이
// 그대로).
Result<void> ReadSourceText(AssetImportContext& context, String& out) noexcept;

// 범위 안의 보간 값인지다.
inline bool IsKnownInterp(CurveInterp interp) noexcept {
    return interp == CurveInterp::Linear || interp == CurveInterp::Constant ||
           interp == CurveInterp::Cubic;
}

} // namespace rocks::curve_import_detail
