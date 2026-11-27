#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/String.h>

// UTF-16 byte 열을 UTF-8로 바꾼다(Docs/Design/CurveAssets 5.1.1절 — 사람이 표 편집기로 저장한
// UTF-16 텍스트 파일 읽기, UE FFileHelper::LoadFileToString이 BOM 있는 UTF-16을 받는 것과 같은
// 용도).
namespace rocks {

// UTF-16 code unit의 byte 순서다.
enum class Utf16ByteOrder : uint8 {
    LittleEndian,
    BigEndian
};

// bytes(BOM 없음 — 호출자가 BOM으로 순서를 정하고 뗀다)를 UTF-8로 out 뒤에 붙인다. surrogate 짝은
// 한 코드 포인트로 합친다. 실패: byte 수가 홀수이거나 짝 없는 surrogate면 InvalidArgument, 할당
// 실패 OutOfMemory. 실패하면 out은 그대로다.
//
// 사용 예:
//   String text(allocator);
//   if (auto converted = TryAppendUtf16AsUtf8(body, Utf16ByteOrder::LittleEndian, text);
//   !converted) {
//       return converted;
//   }
ROCKS_CORE_API Result<void> TryAppendUtf16AsUtf8(Span<const uint8> bytes, Utf16ByteOrder order,
                                                 String& out) noexcept;

} // namespace rocks
