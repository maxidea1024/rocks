#include <MaterialSchema/MaterialGuidPinKey.h>

// Guid 동적 핀 키다(Docs/Design/MaterialSystem 19.13.3절).
namespace rocks::material {

namespace {

// Guid 하나의 16진 자릿수다.
constexpr usize GuidPinKeyHexDigits = 32;
// 16진 한 자리의 bit 수와 mask다.
constexpr int GuidPinKeyHexBits = 4;
constexpr uint64 GuidPinKeyHexMask = 0xF;
// Guid 반쪽(uint64)의 16진 자릿수다.
constexpr int GuidPinKeyHalfDigits = static_cast<int>(GuidPinKeyHexDigits / 2);

} // namespace

Result<Name> MaterialGuidPinKey(NamePool& names, StringView prefix, Guid id) noexcept {
    if (prefix.ByteCount() > MaxMaterialGuidPinKeyPrefixBytes) {
        return Result<Name>::Failure(ErrorCode::InvalidArgument);
    }

    constexpr char hex[] = "0123456789abcdef";
    char buffer[MaxMaterialGuidPinKeyPrefixBytes + GuidPinKeyHexDigits];
    usize length = 0;
    for (; length < prefix.ByteCount(); ++length) {
        buffer[length] = prefix.Data()[length];
    }

    const uint64 halves[] = {id.high, id.low};
    for (uint64 half : halves) {
        for (int digit = GuidPinKeyHalfDigits - 1; digit >= 0; --digit) {
            buffer[length++] = hex[(half >> (digit * GuidPinKeyHexBits)) & GuidPinKeyHexMask];
        }
    }

    return names.TryIntern(StringView(buffer, length));
}

} // namespace rocks::material
