#include <Core/Text/StringBuilder.h>
#include <Core/Text/Utf16.h>

// UTF-16 → UTF-8 변환이다(Core/Text/Utf16.h).
namespace rocks {

namespace {

constexpr uint32 HighSurrogateFirst = 0xD800;
constexpr uint32 HighSurrogateLast = 0xDBFF;
constexpr uint32 LowSurrogateFirst = 0xDC00;
constexpr uint32 LowSurrogateLast = 0xDFFF;
constexpr uint32 SupplementaryFirst = 0x10000;
constexpr uint32 SurrogateBits = 10;
uint32 UnitAt(Span<const uint8> bytes, usize index, Utf16ByteOrder order) noexcept {
    const uint32 first = bytes[index];
    const uint32 second = bytes[index + 1];
    return order == Utf16ByteOrder::LittleEndian ? (first | (second << 8))
                                                 : ((first << 8) | second);
}

} // namespace

Result<void> TryAppendUtf16AsUtf8(Span<const uint8> bytes, Utf16ByteOrder order,
                                  String& out) noexcept {
    if (bytes.Size() % 2 != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 사본에 다 만든 뒤 붙인다(실패면 out 그대로).
    StringBuilder<> staged(out.GetAllocator());
    for (usize index = 0; index < bytes.Size(); index += 2) {
        uint32 codePoint = UnitAt(bytes, index, order);
        if (codePoint >= LowSurrogateFirst && codePoint <= LowSurrogateLast) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (codePoint >= HighSurrogateFirst && codePoint <= HighSurrogateLast) {
            if (index + 3 >= bytes.Size()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            const uint32 low = UnitAt(bytes, index + 2, order);
            if (low < LowSurrogateFirst || low > LowSurrogateLast) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            codePoint = SupplementaryFirst + ((codePoint - HighSurrogateFirst) << SurrogateBits) +
                        (low - LowSurrogateFirst);
            index += 2;
        }

        if (auto appended = staged.TryAppendCodePoint(codePoint); !appended) {
            return appended;
        }
    }

    return out.TryAppend(staged.View());
}

} // namespace rocks
