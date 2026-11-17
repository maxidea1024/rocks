#pragma once

#include <Core/Hash.h>
#include <Core/Text/StringView.h>

// Json.cpp·JsonParser.cpp·JsonWriter.cpp가 같이 쓰는 내부 도구다(공개 API가 아니다).
namespace rocks::detail {

// (소유 객체, 키 byte)의 색인 해시다. 키 byte는 seed가 들어간 SipHash라 seed를 모르는 입력 작성자가
// 칸 충돌을 고를 수 없다. 소유 객체 번호는 그 뒤에 섞는다(같은 키라도 객체마다 다른 칸 — 섞는 값이
// 예측할 수 없는 키 해시라 객체 번호를 골라도 충돌을 맞출 수 없다).
inline uint64 HashKeyBytes(const SeededHash<StringView>& hash, uint32 owner, const char* bytes,
                           usize length) noexcept {
    return HashCombine(hash(StringView(bytes, length)), owner);
}

inline bool Overlaps(const char* data, usize size, const char* begin, usize count) noexcept {
    const auto value = reinterpret_cast<usize>(data);
    const auto start = reinterpret_cast<usize>(begin);
    return size != 0 && count != 0 && value < start + count && start < value + size;
}

} // namespace rocks::detail
