#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// CSV 읽기다(Docs/Design/CurveAssets 5.2절, UE FCsvParser — Runtime/Core/Private/Serialization/Csv/
// CsvParser.cpp와 같은 규칙). 같은 파일을 UE와 같은 칸으로 나눈다. RFC 4180보다 관대하다:
// - 행 첫머리의 줄 끝은 빈 줄로 건너뛴다(공백만 있는 줄은 칸 하나짜리 행이다).
// - 따옴표는 칸 맨 앞에서만 연다. 따옴표 안에서 `""`는 따옴표 하나이고, 따옴표가 닫힌 뒤의 글자는
//   칸에 이어 붙인다(`"ab"cd` → `abcd`). 닫힌 뒤의 따옴표는 글자 그대로다.
// - 닫히지 않은 따옴표는 문자열 끝까지(줄바꿈·쉼표 포함) 한 칸으로 삼킨다(오류가 아니다).
// - 줄 끝은 `\r\n`·`\r`·`\n`이고, 끝 쉼표는 빈 칸 하나를 더한다. 칸은 trim하지 않는다.
// - NUL byte는 문자열 끝이다(UE는 NUL로 끝나는 TCHAR 문자열을 읽는다).
// 입력 인코딩은 보지 않는다(byte 단위 — UTF-8 검증·BOM 처리는 호출자가 한다).
//
// 언제 쓰나: 사람이 표 편집기로 쓴 데이터(커브 표·데이터 표 임포트). 쓰기(내보내기)는 아직 없다.
namespace rocks {

// 행 하나다. line은 이 행이 시작하는 원본의 물리 줄 번호(1부터 — 빈 줄·따옴표 안 줄바꿈도 센다)다.
struct CsvRow {
    explicit CsvRow(Allocator& allocator) noexcept : cells(allocator) {
    }

    Array<String> cells;
    usize line = 0;
};

// text를 행·칸으로 나눠 out에 붙인다(out의 allocator로 만든다). 빈 문자열은 행이 없다. 실패: 할당
// 실패 OutOfMemory(out은 부르기 전 그대로).
//
// 사용 예:
//   Array<CsvRow> rows(allocator);
//   if (auto parsed = TryParseCsv(text, rows); !parsed) { return parsed; }
//   for (const CsvRow& row : rows) { StringView first = row.cells[0].View(); }
ROCKS_CORE_API Result<void> TryParseCsv(StringView text, Array<CsvRow>& out) noexcept;

} // namespace rocks
