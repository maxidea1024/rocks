#include <SparksCompiler/Literal.h>

#include <Base/NumericLimits.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>

namespace rocks::sparks {

namespace {

// 구분자를 뺀 실수 원문 사본의 inline 크기다(넘치면 allocator에서 늘린다).
constexpr usize FloatTextInlineBytes = 64;

constexpr uint32 HexBase = 16;
constexpr uint32 BinaryBase = 2;
constexpr uint32 DecimalBase = 10;

constexpr uint32 DigitValue(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<uint32>(value - '0');
    }

    constexpr uint32 LetterBase = 10;
    constexpr char LowerCaseBit = 0x20;
    return static_cast<uint32>((value | LowerCaseBit) - 'a') + LetterBase;
}

} // namespace

Result<uint64> TryDecodeIntegerLiteral(StringView text) noexcept {
    constexpr usize PrefixBytes = 2;
    uint32 base = DecimalBase;
    usize index = 0;
    if (text.ByteCount() > PrefixBytes && text.Data()[0] == '0') {
        const char marker = text.Data()[1];
        if (marker == 'x' || marker == 'X') {
            base = HexBase;
            index = PrefixBytes;
        } else if (marker == 'b' || marker == 'B') {
            base = BinaryBase;
            index = PrefixBytes;
        }
    }

    uint64 value = 0;
    constexpr uint64 Max = NumericLimits<uint64>::Max();
    for (; index < text.ByteCount(); ++index) {
        const char character = text.Data()[index];
        if (character == '_') {
            continue;
        }

        const uint32 digit = DigitValue(character);
        if (value > (Max - digit) / base) {
            return Result<uint64>::Failure(ErrorCode::Overflow);
        }

        value = value * base + digit;
    }

    return Result<uint64>::Success(value);
}

Result<double> TryDecodeFloatLiteral(Allocator& allocator, StringView text) noexcept {
    StringBuilder<FloatTextInlineBytes> digits(allocator);
    usize runBegin = 0;
    for (usize index = 0; index <= text.ByteCount(); ++index) {
        if (index < text.ByteCount() && text.Data()[index] != '_') {
            continue;
        }

        if (auto appended = digits.TryAppend(StringView(text.Data() + runBegin, index - runBegin));
            !appended) {
            return Result<double>::Failure(appended.Error());
        }

        runBegin = index + 1;
    }

    return TryParseDouble(digits.View());
}

} // namespace rocks::sparks
