#include <Core/Text/Json.h>

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

// ---- 값 ----

bool JsonValue::IsValid() const noexcept {
    return document_ != nullptr && document_->generation_ == generation_;
}

void JsonValue::CheckValid(const char* operation) const noexcept {
    if (!IsValid()) {
        platform::Fatal(operation);
    }
}

JsonType JsonValue::Type() const noexcept {
    CheckValid("JsonValue::Type on an invalid or stale value");
    return document_->nodes_[node_].type;
}

bool JsonValue::IsNull() const noexcept {
    return IsValid() && Type() == JsonType::Null;
}

bool JsonValue::IsBool() const noexcept {
    return IsValid() && Type() == JsonType::Bool;
}

bool JsonValue::IsNumber() const noexcept {
    if (!IsValid()) {
        return false;
    }

    const JsonType type = Type();
    return type == JsonType::Integer || type == JsonType::Unsigned || type == JsonType::Double;
}

bool JsonValue::IsString() const noexcept {
    return IsValid() && Type() == JsonType::String;
}

bool JsonValue::IsArray() const noexcept {
    return IsValid() && Type() == JsonType::Array;
}

bool JsonValue::IsObject() const noexcept {
    return IsValid() && Type() == JsonType::Object;
}

bool JsonValue::GetBool() const noexcept {
    if (!IsBool()) {
        platform::Fatal("JsonValue::GetBool on a non-bool, invalid or stale value");
    }

    return document_->nodes_[node_].boolean;
}

Result<int64> JsonValue::TryGetInt64() const noexcept {
    if (IsValid()) {
        const JsonDocument::Node& node = document_->nodes_[node_];
        if (node.type == JsonType::Integer) {
            return Result<int64>::Success(static_cast<int64>(node.bits));
        }
    }

    return Result<int64>::Failure(ErrorCode::InvalidArgument);
}

Result<uint64> JsonValue::TryGetUInt64() const noexcept {
    if (IsValid()) {
        const JsonDocument::Node& node = document_->nodes_[node_];
        if (node.type == JsonType::Unsigned ||
            (node.type == JsonType::Integer && static_cast<int64>(node.bits) >= 0)) {
            return Result<uint64>::Success(node.bits);
        }
    }

    return Result<uint64>::Failure(ErrorCode::InvalidArgument);
}

Result<double> JsonValue::TryGetDouble() const noexcept {
    if (IsValid()) {
        const JsonDocument::Node& node = document_->nodes_[node_];
        switch (node.type) {
        case JsonType::Integer:
            return Result<double>::Success(static_cast<double>(static_cast<int64>(node.bits)));
        case JsonType::Unsigned:
            return Result<double>::Success(static_cast<double>(node.bits));
        case JsonType::Double:
            return Result<double>::Success(platform::BitCast<double>(node.bits));
        default:
            break;
        }
    }

    return Result<double>::Failure(ErrorCode::InvalidArgument);
}

JsonPosition JsonValue::Position() const noexcept {
    CheckValid("JsonValue::Position on an invalid or stale value");
    const JsonDocument::NodePosition& entry = document_->positions_[node_];
    JsonPosition position;
    position.line = entry.line;
    position.column = entry.column;
    position.byteOffset = entry.byteOffset;
    return position;
}

StringView JsonValue::RawNumber() const noexcept {
    if (!IsNumber()) {
        platform::Fatal("JsonValue::RawNumber on a non-number, invalid or stale value");
    }

    const JsonDocument::Node& node = document_->nodes_[node_];
    return document_->Text(node.first, node.count);
}

StringView JsonValue::GetString() const noexcept {
    if (!IsString()) {
        platform::Fatal("JsonValue::GetString on a non-string, invalid or stale value");
    }

    const JsonDocument::Node& node = document_->nodes_[node_];
    return document_->Text(node.first, node.count);
}

usize JsonValue::Size() const noexcept {
    if (!IsArray() && !IsObject()) {
        return 0;
    }

    return document_->nodes_[node_].count;
}

JsonValue JsonValue::At(usize index) const noexcept {
    if (!IsArray() || index >= Size()) {
        platform::Fatal("JsonValue::At outside an array");
    }

    return JsonValue(document_, document_->elements_[document_->nodes_[node_].first + index],
                     generation_);
}

StringView JsonValue::KeyAt(usize index) const noexcept {
    if (!IsObject() || index >= Size()) {
        platform::Fatal("JsonValue::KeyAt outside an object");
    }

    const JsonDocument::Member& member =
        document_->members_[document_->nodes_[node_].first + index];
    return document_->Text(member.keyOffset, member.keyLength);
}

JsonValue JsonValue::ValueAt(usize index) const noexcept {
    if (!IsObject() || index >= Size()) {
        platform::Fatal("JsonValue::ValueAt outside an object");
    }

    return JsonValue(document_, document_->members_[document_->nodes_[node_].first + index].value,
                     generation_);
}

JsonValue JsonValue::Find(StringView key) const noexcept {
    if (!IsObject()) {
        return JsonValue();
    }

    const uint32* member = document_->index_.Find(JsonDocument::MemberQuery{node_, key});
    return member == nullptr
               ? JsonValue()
               : JsonValue(document_, document_->members_[*member].value, generation_);
}

namespace {

// 숫자 노드의 정확한 값: 부호와 크기(정수) 또는 "정수가 아니거나 2^64 이상".
struct ExactInteger {
    bool integral; // 2^64 미만의 정수인가
    bool negative;
    uint64 magnitude;
};

// double 비트를 정수 연산으로 풀어 정확한 정수인지 본다(부동소수점 환경과 무관).
ExactInteger DoubleAsInteger(uint64 bits) noexcept {
    const bool negative = (bits >> 63) != 0;
    const uint32 biased = static_cast<uint32>((bits >> 52) & 0x7FF);
    const uint64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (biased == 0 && fraction == 0) {
        return {true, false, 0}; // ±0은 0
    }

    if (biased == 0) {
        return {false, negative, 0}; // 비정규 수는 0과 1 사이
    }

    const uint64 significand = fraction | (uint64{1} << 52);
    const int32 shift = static_cast<int32>(biased) - 1075; // 값 = significand·2^shift
    if (shift >= 0) {
        if (shift > 11) {
            return {false, negative, 0}; // 2^64 이상
        }

        return {true, negative, significand << shift};
    }

    if (shift <= -64) {
        return {false, negative, 0};
    }

    const uint64 dropped = significand & ((uint64{1} << -shift) - 1u);
    return {dropped == 0, negative, significand >> -shift};
}

ExactInteger IntegerOf(JsonType type, uint64 bits) noexcept {
    if (type == JsonType::Unsigned) {
        return {true, false, bits};
    }

    const int64 value = static_cast<int64>(bits);
    return value < 0 ? ExactInteger{true, true, static_cast<uint64>(-(value + 1)) + 1u}
                     : ExactInteger{true, false, bits};
}

// 두 숫자 노드의 수치 비교(정수 연산만). double끼리는 비트가 같거나 둘 다 ±0이면 같다. Json5의
// ±Infinity·NaN도 비트로 비교한다(NaN은 파서가 한 비트 패턴으로 저장한다). DoubleAsInteger는 이들을
// 정수가 아니라고 보므로 정수 종류와는 같지 않다.
bool SameNumber(JsonType leftType, uint64 leftBits, JsonType rightType, uint64 rightBits) noexcept {
    const bool leftDouble = leftType == JsonType::Double;
    const bool rightDouble = rightType == JsonType::Double;
    if (leftDouble && rightDouble) {
        return leftBits == rightBits || ((leftBits | rightBits) << 1) == 0;
    }

    const ExactInteger left =
        leftDouble ? DoubleAsInteger(leftBits) : IntegerOf(leftType, leftBits);
    const ExactInteger right =
        rightDouble ? DoubleAsInteger(rightBits) : IntegerOf(rightType, rightBits);
    if (!left.integral || !right.integral) {
        return false;
    }

    return left.magnitude == right.magnitude &&
           (left.negative == right.negative || left.magnitude == 0);
}

bool IsNumberType(JsonType type) noexcept {
    return type == JsonType::Integer || type == JsonType::Unsigned || type == JsonType::Double;
}

} // namespace

bool JsonEquals(JsonValue left, JsonValue right) noexcept {
    if (!left.IsValid() || !right.IsValid()) {
        return left.IsValid() == right.IsValid();
    }

    // 서브트리는 노드 배열의 연속 구간(전위 순서)이다. 컨테이너의 원소·멤버 수까지 같은 두 전위
    // 열은 같은 트리이므로 두 구간을 나란히 한 번 훑으면 된다(재귀 없음).
    const JsonDocument& leftDocument = *left.document_;
    const JsonDocument& rightDocument = *right.document_;
    uint32 a = left.node_;
    uint32 b = right.node_;
    const uint32 leftEnd = leftDocument.nodes_[a].end;
    if (leftEnd - a != rightDocument.nodes_[b].end - b) {
        return false;
    }

    for (; a < leftEnd; ++a, ++b) {
        const JsonDocument::Node& x = leftDocument.nodes_[a];
        const JsonDocument::Node& y = rightDocument.nodes_[b];
        if (IsNumberType(x.type) && IsNumberType(y.type)) {
            if (!SameNumber(x.type, x.bits, y.type, y.bits)) {
                return false;
            }

            continue;
        }

        if (x.type != y.type) {
            return false;
        }

        switch (x.type) {
        case JsonType::Bool:
            if (x.boolean != y.boolean) {
                return false;
            }

            break;
        case JsonType::String:
            if (leftDocument.Text(x.first, x.count) != rightDocument.Text(y.first, y.count)) {
                return false;
            }

            break;
        case JsonType::Array:
            if (x.count != y.count) {
                return false;
            }

            break;
        case JsonType::Object:
            if (x.count != y.count) {
                return false;
            }

            for (uint32 index = 0; index < x.count; ++index) {
                const JsonDocument::Member& p = leftDocument.members_[x.first + index];
                const JsonDocument::Member& q = rightDocument.members_[y.first + index];
                if (leftDocument.Text(p.keyOffset, p.keyLength) !=
                    rightDocument.Text(q.keyOffset, q.keyLength)) {
                    return false;
                }
            }

            break;
        default:
            break; // Null
        }
    }

    return true;
}

} // namespace rocks
