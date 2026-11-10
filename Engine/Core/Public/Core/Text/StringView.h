#pragma once

#include <Base/Types.h>
#include <Core/Result.h>

namespace rocks {

// 원본 byte 범위를 빌리는 view다. UTF-8 유효성을 생성 시 자동 검사하지 않으므로
// 외부 입력은 ValidateUtf8로 검증한다. 원본 파괴·재할당·내용 변경은 caller가 통제한다.
// 길이는 code point나 grapheme 수가 아닌 byte 수이며 동등 비교도 정규화 없는 byte 비교다.
class StringView {
public:
    constexpr StringView() noexcept = default;

    // C 문자열의 첫 NUL까지 읽는다. nullptr는 빈 view다.
    // embedded NUL을 포함하려면 아래 pointer/byteCount 생성자를 사용한다.
    constexpr StringView(const char* nullTerminated) noexcept : data_(nullTerminated) {
        if (data_ != nullptr) {
            while (data_[bytes_] != '\0') {
                ++bytes_;
            }
        }
    }

    // caller는 byteCount만큼 읽을 수 있는 storage를 제공한다. nullptr는 길이 0에만 허용한다.
    constexpr StringView(const char* data, usize byteCount) noexcept
        : data_(data), bytes_(byteCount) {
        if (data_ == nullptr && bytes_ != 0) {
            platform::Fatal("A nonempty StringView requires storage");
        }
    }

    constexpr const char* Data() const noexcept {
        return data_;
    }

    constexpr usize ByteCount() const noexcept {
        return bytes_;
    }

    constexpr bool IsEmpty() const noexcept {
        return bytes_ == 0;
    }

    // Unicode scalar의 UTF-8만 허용한다. overlong·surrogate·범위 초과·잘린 sequence는
    // InvalidArgument다. NUL은 유효한 U+0000이며 이 검사는 할당이나 정규화를 하지 않는다.
    Result<void> ValidateUtf8() const noexcept;

    // 원본 전체가 유효한 UTF-8인지와 양 끝이 code point 경계인지 검사한다.
    // byte 범위 초과·문자 중간 절단은 InvalidArgument다. 빈 slice도 UTF 경계가 필요하다.
    // 반환 view는 새 storage를 만들지 않으며 grapheme 경계까지 보장하지 않는다.
    Result<StringView> TrySliceBytes(usize offset, usize byteCount) const noexcept;

    friend constexpr bool operator==(StringView left, StringView right) noexcept {
        if (left.bytes_ != right.bytes_) {
            return false;
        }

        for (usize index = 0; index < left.bytes_; ++index) {
            if (left.data_[index] != right.data_[index]) {
                return false;
            }
        }

        return true;
    }

private:
    const char* data_ = nullptr;
    usize bytes_ = 0;
};

} // namespace rocks
