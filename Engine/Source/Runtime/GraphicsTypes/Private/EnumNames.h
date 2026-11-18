#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/Memory.h>

// 열거값 이름 조회의 모듈 내부 공용 도구다(Sampler·CompareFunction·TextureDimension·
// ChannelSwizzle의 Parse 함수).
namespace rocks::graphics::detail {

// ASCII 대소문자를 무시하고 text가 name과 같은지다.
inline bool EqualsIgnoringCase(StringView text, const char* name) noexcept {
    const usize nameBytes = platform::CStringLength(name);
    if (text.ByteCount() != nameBytes) {
        return false;
    }
    for (usize index = 0; index < nameBytes; ++index) {
        char left = text.Data()[index];
        char right = name[index];
        left = left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
        right = right >= 'A' && right <= 'Z' ? static_cast<char>(right - 'A' + 'a') : right;
        if (left != right) {
            return false;
        }
    }

    return true;
}

// 0부터 count개 열거값의 이름(nameOf)과 대조한다. 없으면 InvalidArgument다.
template <class Enum>
Result<Enum> ParseByName(StringView name, uint32 count, const char* (*nameOf)(Enum)) noexcept {
    for (uint32 value = 0; value < count; ++value) {
        if (EqualsIgnoringCase(name, nameOf(static_cast<Enum>(value)))) {
            return Result<Enum>::Success(static_cast<Enum>(value));
        }
    }

    return Result<Enum>::Failure(ErrorCode::InvalidArgument);
}

} // namespace rocks::graphics::detail
