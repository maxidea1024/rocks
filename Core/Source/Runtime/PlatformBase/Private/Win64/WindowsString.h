#pragma once

#include <Platform/File.h>
#include <Platform/Win64/WideString.h>

#include <Windows.h>

// PlatformBase 공개 API가 문자열을 호출자 버퍼로 돌려주는 규칙이다. UTF-8·UTF-16 변환 자체는 공개
// 헤더 Platform/Win64/WideString.h에 있고, 여기는 그 결과를 공개 API 계약의 오류 코드로 옮기기만
// 한다.
namespace rocks::platform::detail {

// GetStandardDirectory·GetExecutablePath의 반환 규칙이다. UTF-16 units개를 UTF-8로 buffer에
// 쓰고(NUL 포함) 길이를 bytes에 둔다(NUL 제외). buffer가 부족하면 Overflow이며 bytes에 필요한
// 길이를 둔다. 바꿀 수 없는 UTF-16이면 SystemError(ERROR_NO_UNICODE_TRANSLATION)다(OS가 준 경로가
// 잘못된 것이므로 호출자 인자 오류가 아니다).
inline FileStatus WriteUtf8(const wchar_t* text, usize units, char* buffer, usize capacity,
                            usize& bytes) noexcept {
    const windows::TextConversion converted =
        windows::ConvertWideToUtf8(text, units, buffer, capacity, windows::TextFit::Exact);
    if (converted) {
        bytes = converted.length;
        return {};
    }

    if (converted.status.code == FileCode::Overflow) {
        bytes = converted.required;
        return {FileCode::Overflow, 0};
    }

    return {FileCode::SystemError, ERROR_NO_UNICODE_TRANSLATION};
}

} // namespace rocks::platform::detail
