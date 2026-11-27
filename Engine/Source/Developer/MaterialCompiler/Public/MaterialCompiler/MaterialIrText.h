#pragma once

#include <Base/Types.h>

// IR 표가 소유한 문자열 하나의 범위다(Docs/Design/MaterialSystem 6.13절 MS2f5 — Custom 함수 표).
// 표의 문자 저장소 안 byte 위치·길이이고, 원문은 그 표의 Text(IrText)로 읽는다. 표를 옮기거나
// 복사해도 범위는 그대로다(포인터가 아니라서).
//
// 사용 예:
//   const StringView code = table.Text(function.code);
namespace rocks::material {

struct IrText {
    uint32 offset = 0;
    uint32 size = 0;
};

} // namespace rocks::material
