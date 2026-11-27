#pragma once

#include <Base/NumericLimits.h>
#include <Core/Text/Json.h>

// 교환 JSON 값 읽기 도우미다(UnrealExchange 모듈 안에서만 쓴다). 무효 값(없는 키의 Find)의 접근자는
// Fatal이므로 종류를 먼저 보고 읽는다. 모두 실패하면 false이고 out은 그대로다.
namespace rocks::exchange_detail {

// object의 key 멤버다. object가 객체가 아니거나 키가 없으면 무효 값이다.
inline JsonValue Member(JsonValue object, StringView key) noexcept {
    return object.IsObject() ? object.Find(key) : JsonValue();
}

inline bool ReadUInt(JsonValue value, uint64& out) noexcept {
    if (!value.IsNumber()) {
        return false;
    }

    auto read = value.TryGetUInt64();
    if (!read) {
        return false;
    }

    out = read.Value();
    return true;
}

inline bool ReadInt32(JsonValue value, int32& out) noexcept {
    if (!value.IsNumber()) {
        return false;
    }

    auto read = value.TryGetInt64();
    if (!read || read.Value() < NumericLimits<int32>::Min() ||
        read.Value() > NumericLimits<int32>::Max()) {
        return false;
    }

    out = static_cast<int32>(read.Value());
    return true;
}

inline bool ReadBool(JsonValue value, bool& out) noexcept {
    if (!value.IsBool()) {
        return false;
    }

    out = value.GetBool();
    return true;
}

inline bool ReadString(JsonValue value, StringView& out) noexcept {
    if (!value.IsString()) {
        return false;
    }

    out = value.GetString();
    return true;
}

} // namespace rocks::exchange_detail
